/**
 *    Copyright (C) 2019-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/query/sbe_stage_builder.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/exec/sbe/stages/filter.h"
#include "mongo/db/exec/sbe/stages/hash_agg.h"
#include "mongo/db/exec/sbe/stages/hash_join.h"
#include "mongo/db/exec/sbe/stages/limit_skip.h"
#include "mongo/db/exec/sbe/stages/loop_join.h"
#include "mongo/db/exec/sbe/stages/makeobj.h"
#include "mongo/db/exec/sbe/stages/merge_join.h"
#include "mongo/db/exec/sbe/stages/project.h"
#include "mongo/db/exec/sbe/stages/scan.h"
#include "mongo/db/exec/sbe/stages/sort.h"
#include "mongo/db/exec/sbe/stages/sorted_merge.h"
#include "mongo/db/exec/sbe/stages/traverse.h"
#include "mongo/db/exec/sbe/stages/union.h"
#include "mongo/db/exec/sbe/stages/unique.h"
#include "mongo/db/exec/sbe/values/sort_spec.h"
#include "mongo/db/exec/shard_filterer.h"
#include "mongo/db/fts/fts_index_format.h"
#include "mongo/db/fts/fts_query_impl.h"
#include "mongo/db/fts/fts_spec.h"
#include "mongo/db/index/fts_access_method.h"
#include "mongo/db/query/sbe_stage_builder_coll_scan.h"
#include "mongo/db/query/sbe_stage_builder_filter.h"
#include "mongo/db/query/sbe_stage_builder_helpers.h"
#include "mongo/db/query/sbe_stage_builder_index_scan.h"
#include "mongo/db/query/sbe_stage_builder_projection.h"
#include "mongo/db/query/util/make_data_structure.h"
#include "mongo/db/s/collection_sharding_state.h"

namespace mongo::stage_builder {
namespace {
/**
 * Tree representation of an index key pattern.
 *
 * For example, the key pattern {a.b: 1, x: 1, a.c: 1} would look like:
 *
 *         <root>
 *         /   |
 *        a    x
 *       / \
 *      b   c
 *
 * This tree is used for building SBE subtrees to re-hydrate index keys.
 */
struct IndexKeyPatternTreeNode {
    IndexKeyPatternTreeNode* emplace(StringData fieldComponent) {
        auto newNode = std::make_unique<IndexKeyPatternTreeNode>();
        const auto newNodeRaw = newNode.get();
        children.emplace(fieldComponent, std::move(newNode));
        childrenOrder.push_back(fieldComponent.toString());

        return newNodeRaw;
    }

    StringMap<std::unique_ptr<IndexKeyPatternTreeNode>> children;
    std::vector<std::string> childrenOrder;

    // Which slot the index key for this component is stored in. May be boost::none for non-leaf
    // nodes.
    boost::optional<sbe::value::SlotId> indexKeySlot;
};

/**
 * Given a key pattern and an array of slots of equal size, builds an IndexKeyPatternTreeNode
 * representing the mapping between key pattern component and slot.
 *
 * Note that this will "short circuit" in cases where the index key pattern contains two components
 * where one is a subpath of the other. For example with the key pattern {a:1, a.b: 1}, the "a.b"
 * component will not be represented in the output tree. For the purpose of rehydrating index keys,
 * this is fine (and actually preferable).
 */
std::unique_ptr<IndexKeyPatternTreeNode> buildKeyPatternTree(const BSONObj& keyPattern,
                                                             const sbe::value::SlotVector& slots) {
    size_t i = 0;

    auto root = std::make_unique<IndexKeyPatternTreeNode>();
    for (auto&& elem : keyPattern) {
        auto* node = root.get();
        bool skipElem = false;

        FieldRef fr(elem.fieldNameStringData());
        for (FieldIndex j = 0; j < fr.numParts(); ++j) {
            const auto part = fr.getPart(j);
            if (auto it = node->children.find(part); it != node->children.end()) {
                node = it->second.get();
                if (node->indexKeySlot) {
                    // We're processing the a sub-path of a path that's already indexed.  We can
                    // bail out here since we won't use the sub-path when reconstructing the
                    // object.
                    skipElem = true;
                    break;
                }
            } else {
                node = node->emplace(part);
            }
        }

        if (!skipElem) {
            node->indexKeySlot = slots[i];
        }

        ++i;
    }

    return root;
}

/**
 * Given a root IndexKeyPatternTreeNode, this function will construct an SBE expression for
 * producing a partial object from an index key.
 *
 * For example, given the index key pattern {a.b: 1, x: 1, a.c: 1} and the index key
 * {"": 1, "": 2, "": 3}, the SBE expression would produce the object {a: {b:1, c: 3}, x: 2}.
 */
std::unique_ptr<sbe::EExpression> buildNewObjExpr(const IndexKeyPatternTreeNode* kpTree) {

    std::vector<std::unique_ptr<sbe::EExpression>> args;
    for (auto&& fieldName : kpTree->childrenOrder) {
        auto it = kpTree->children.find(fieldName);

        args.emplace_back(makeConstant(fieldName));
        if (it->second->indexKeySlot) {
            args.emplace_back(makeVariable(*it->second->indexKeySlot));
        } else {
            // The reason this is in an else branch is that in the case where we have an index key
            // like {a.b: ..., a: ...}, we've already made the logic for reconstructing the 'a'
            // portion, so the 'a.b' subtree can be skipped.
            args.push_back(buildNewObjExpr(it->second.get()));
        }
    }

    return sbe::makeE<sbe::EFunction>("newObj", std::move(args));
}

/**
 * Given a stage, and index key pattern a corresponding array of slot IDs, this function
 * add a ProjectStage to the tree which rehydrates the index key and stores the result in
 * 'resultSlot.'
 */
std::unique_ptr<sbe::PlanStage> rehydrateIndexKey(std::unique_ptr<sbe::PlanStage> stage,
                                                  const BSONObj& indexKeyPattern,
                                                  PlanNodeId nodeId,
                                                  const sbe::value::SlotVector& indexKeySlots,
                                                  sbe::value::SlotId resultSlot) {
    auto kpTree = buildKeyPatternTree(indexKeyPattern, indexKeySlots);
    auto keyExpr = buildNewObjExpr(kpTree.get());

    return sbe::makeProjectStage(std::move(stage), nodeId, resultSlot, std::move(keyExpr));
}

/**
 * Generates an EOF plan. Note that even though this plan will return nothing, it will still define
 * the slots specified by 'reqs'.
 */
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> generateEofPlan(
    PlanNodeId nodeId, const PlanStageReqs& reqs, sbe::value::SlotIdGenerator* slotIdGenerator) {
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projects;

    PlanStageSlots outputs(reqs, slotIdGenerator);
    outputs.forEachSlot(reqs, [&](auto&& slot) {
        projects.insert({slot, sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Nothing, 0)});
    });

    auto stage = sbe::makeS<sbe::LimitSkipStage>(
        sbe::makeS<sbe::CoScanStage>(nodeId), 0, boost::none, nodeId);

    if (!projects.empty()) {
        // Even though this SBE tree will produce zero documents, we still need a ProjectStage to
        // define the slots in 'outputSlots' so that calls to getAccessor() won't fail.
        stage = sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projects), nodeId);
    }

    return {std::move(stage), std::move(outputs)};
}
}  // namespace

std::unique_ptr<sbe::RuntimeEnvironment> makeRuntimeEnvironment(
    const CanonicalQuery& cq,
    OperationContext* opCtx,
    sbe::value::SlotIdGenerator* slotIdGenerator) {
    auto env = std::make_unique<sbe::RuntimeEnvironment>();

    // Register an unowned global timezone database for datetime expression evaluation.
    env->registerSlot("timeZoneDB"_sd,
                      sbe::value::TypeTags::timeZoneDB,
                      sbe::value::bitcastFrom<const TimeZoneDatabase*>(getTimeZoneDatabase(opCtx)),
                      false,
                      slotIdGenerator);

    if (auto collator = cq.getCollator(); collator) {
        env->registerSlot("collator"_sd,
                          sbe::value::TypeTags::collator,
                          sbe::value::bitcastFrom<const CollatorInterface*>(collator),
                          false,
                          slotIdGenerator);
    }

    return env;
}

PlanStageSlots::PlanStageSlots(const PlanStageReqs& reqs,
                               sbe::value::SlotIdGenerator* slotIdGenerator) {
    for (auto&& [slotName, isRequired] : reqs._slots) {
        if (isRequired) {
            _slots[slotName] = slotIdGenerator->generate();
        }
    }
}

std::string PlanStageData::debugString() const {
    StringBuilder builder;

    if (auto slot = outputs.getIfExists(PlanStageSlots::kResult); slot) {
        builder << "$$RESULT=s" << *slot << " ";
    }
    if (auto slot = outputs.getIfExists(PlanStageSlots::kRecordId); slot) {
        builder << "$$RID=s" << *slot << " ";
    }
    if (auto slot = outputs.getIfExists(PlanStageSlots::kOplogTs); slot) {
        builder << "$$OPLOGTS=s" << *slot << " ";
    }

    env->debugString(&builder);

    return builder.str();
}

namespace {
const QuerySolutionNode* getNodeByType(const QuerySolutionNode* root, StageType type) {
    if (root->getType() == type) {
        return root;
    }

    for (auto&& child : root->children) {
        if (auto result = getNodeByType(child, type)) {
            return result;
        }
    }

    return nullptr;
}

sbe::LockAcquisitionCallback makeLockAcquisitionCallback(bool checkNodeCanServeReads) {
    if (!checkNodeCanServeReads) {
        return {};
    }

    return [](OperationContext* opCtx, const AutoGetCollectionForReadMaybeLockFree& coll) {
        uassertStatusOK(repl::ReplicationCoordinator::get(opCtx)->checkCanServeReadsFor(
            opCtx, coll.getNss(), true));
    };
}

std::unique_ptr<fts::FTSMatcher> makeFtsMatcher(OperationContext* opCtx,
                                                const CollectionPtr& collection,
                                                const std::string& indexName,
                                                const fts::FTSQuery* ftsQuery) {
    auto desc = collection->getIndexCatalog()->findIndexByName(opCtx, indexName);
    tassert(5432209,
            str::stream() << "index descriptor not found for index named '" << indexName
                          << "' in collection '" << collection->ns() << "'",
            desc);

    auto entry = collection->getIndexCatalog()->getEntry(desc);
    tassert(5432210,
            str::stream() << "index entry not found for index named '" << indexName
                          << "' in collection '" << collection->ns() << "'",
            entry);

    auto accessMethod = static_cast<const FTSAccessMethod*>(entry->accessMethod());
    tassert(5432211,
            str::stream() << "access method is not defined for index named '" << indexName
                          << "' in collection '" << collection->ns() << "'",
            accessMethod);

    // We assume here that node->ftsQuery is an FTSQueryImpl, not an FTSQueryNoop. In practice, this
    // means that it is illegal to use the StageBuilder on a QuerySolution created by planning a
    // query that contains "no-op" expressions.
    auto query = dynamic_cast<const fts::FTSQueryImpl*>(ftsQuery);
    tassert(5432220, "expected FTSQueryImpl", query);
    return std::make_unique<fts::FTSMatcher>(*query, accessMethod->getSpec());
}
}  // namespace

SlotBasedStageBuilder::SlotBasedStageBuilder(OperationContext* opCtx,
                                             const CollectionPtr& collection,
                                             const CanonicalQuery& cq,
                                             const QuerySolution& solution,
                                             PlanYieldPolicySBE* yieldPolicy,
                                             ShardFiltererFactoryInterface* shardFiltererFactory)
    : StageBuilder(opCtx, collection, cq, solution),
      _yieldPolicy(yieldPolicy),
      _data(makeRuntimeEnvironment(_cq, _opCtx, &_slotIdGenerator)),
      _shardFiltererFactory(shardFiltererFactory),
      _lockAcquisitionCallback(makeLockAcquisitionCallback(solution.shouldCheckCanServeReads())) {
    // SERVER-52803: In the future if we need to gather more information from the QuerySolutionNode
    // tree, rather than doing one-off scans for each piece of information, we should add a formal
    // analysis pass here.
    if (auto node = getNodeByType(solution.root(), STAGE_COLLSCAN)) {
        auto csn = static_cast<const CollectionScanNode*>(node);
        _data.shouldTrackLatestOplogTimestamp = csn->shouldTrackLatestOplogTimestamp;
        _data.shouldTrackResumeToken = csn->requestResumeToken;
        _data.shouldUseTailableScan = csn->tailable;
    }

    if (auto node = getNodeByType(solution.root(), STAGE_VIRTUAL_SCAN)) {
        auto vsn = static_cast<const VirtualScanNode*>(node);
        _shouldProduceRecordIdSlot = vsn->hasRecordId;
    }
}

std::unique_ptr<sbe::PlanStage> SlotBasedStageBuilder::build(const QuerySolutionNode* root) {
    // For a given SlotBasedStageBuilder instance, this build() method can only be called once.
    invariant(!_buildHasStarted);
    _buildHasStarted = true;

    // We always produce a 'resultSlot' and conditionally produce a 'recordIdSlot' based on the
    // 'shouldProduceRecordIdSlot'. If the solution contains a CollectionScanNode with the
    // 'shouldTrackLatestOplogTimestamp' flag set to true, then we will also produce an
    // 'oplogTsSlot'.
    PlanStageReqs reqs;
    reqs.set(kResult);
    reqs.setIf(kRecordId, _shouldProduceRecordIdSlot);
    reqs.setIf(kOplogTs, _data.shouldTrackLatestOplogTimestamp);

    // Build the SBE plan stage tree.
    auto [stage, outputs] = build(root, reqs);

    // Assert that we produced a 'resultSlot' and that we prouced a 'recordIdSlot' if the
    // 'shouldProduceRecordIdSlot' flag was set. Also assert that we produced an 'oplogTsSlot' if
    // it's needed.
    invariant(outputs.has(kResult));
    invariant(!_shouldProduceRecordIdSlot || outputs.has(kRecordId));
    invariant(!_data.shouldTrackLatestOplogTimestamp || outputs.has(kOplogTs));

    _data.outputs = std::move(outputs);

    return std::move(stage);
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildCollScan(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    invariant(!reqs.getIndexKeyBitset());

    auto csn = static_cast<const CollectionScanNode*>(root);

    auto [stage, outputs] = generateCollScan(_opCtx,
                                             _collection,
                                             csn,
                                             &_slotIdGenerator,
                                             &_frameIdGenerator,
                                             _yieldPolicy,
                                             _data.env,
                                             reqs.getIsTailableCollScanResumeBranch(),
                                             _lockAcquisitionCallback);

    if (reqs.has(kReturnKey)) {
        // Assign the 'returnKeySlot' to be the empty object.
        outputs.set(kReturnKey, _slotIdGenerator.generate());
        stage = sbe::makeProjectStage(std::move(stage),
                                      root->nodeId(),
                                      outputs.get(kReturnKey),
                                      sbe::makeE<sbe::EFunction>("newObj", sbe::makeEs()));
    }

    // Assert that generateCollScan() generated an oplogTsSlot if it's needed.
    invariant(!reqs.has(kOplogTs) || outputs.has(kOplogTs));

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildVirtualScan(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    using namespace std::literals;
    auto vsn = static_cast<const VirtualScanNode*>(root);
    // The caller should only have requested components of the index key if the virtual scan is
    // mocking an index scan.
    if (vsn->scanType == VirtualScanNode::ScanType::kCollScan) {
        invariant(!reqs.getIndexKeyBitset());
    }

    // Virtual scans cannot produce an oplogTsSlot, so assert that the caller doesn't need it.
    invariant(!reqs.has(kOplogTs));

    auto [inputTag, inputVal] = sbe::value::makeNewArray();
    sbe::value::ValueGuard inputGuard{inputTag, inputVal};
    auto inputView = sbe::value::getArrayView(inputVal);

    for (auto& doc : vsn->docs) {
        auto [tag, val] = makeValue(doc);
        inputView->push_back(tag, val);
    }

    inputGuard.reset();
    auto [scanSlots, stage] =
        generateVirtualScanMulti(&_slotIdGenerator, vsn->hasRecordId ? 2 : 1, inputTag, inputVal);

    sbe::value::SlotId resultSlot;
    if (vsn->hasRecordId) {
        invariant(scanSlots.size() == 2);
        resultSlot = scanSlots[1];
    } else {
        invariant(scanSlots.size() == 1);
        resultSlot = scanSlots[0];
    }

    PlanStageSlots outputs;

    if (reqs.has(kResult)) {
        outputs.set(kResult, resultSlot);
    } else if (reqs.getIndexKeyBitset()) {
        // The caller wanted individual slots for certain components of a mock index scan. Use a
        // project stage to produce those slots. Since the test will represent index keys as BSON
        // objects, we use 'getField' expressions to extract the necessary fields.
        invariant(!vsn->indexKeyPattern.isEmpty());

        sbe::value::SlotVector indexKeySlots;
        sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projections;

        size_t indexKeyPos = 0;
        for (auto&& field : vsn->indexKeyPattern) {
            if (reqs.getIndexKeyBitset()->test(indexKeyPos)) {
                indexKeySlots.push_back(_slotIdGenerator.generate());
                projections.emplace(indexKeySlots.back(),
                                    makeFunction("getField"_sd,
                                                 sbe::makeE<sbe::EVariable>(resultSlot),
                                                 makeConstant(field.fieldName())));
            }
            ++indexKeyPos;
        }

        stage =
            sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projections), root->nodeId());

        outputs.setIndexKeySlots(indexKeySlots);
    }

    if (reqs.has(kRecordId)) {
        invariant(vsn->hasRecordId);
        invariant(scanSlots.size() == 2);
        outputs.set(kRecordId, scanSlots[0]);
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildIndexScan(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto ixn = static_cast<const IndexScanNode*>(root);
    invariant(reqs.has(kReturnKey) || !ixn->addKeyMetadata);

    // Index scans cannot produce an oplogTsSlot, so assert that the caller doesn't need it.
    invariant(!reqs.has(kOplogTs));

    sbe::IndexKeysInclusionSet indexKeyBitset;

    if (reqs.has(PlanStageSlots::kReturnKey) || reqs.has(PlanStageSlots::kResult)) {
        // If either 'reqs.result' or 'reqs.returnKey' is true, we need to get all parts of the
        // index key (regardless of what was requested by 'reqs.indexKeyBitset') so that we can
        // create the inflated index key (keyExpr).
        for (int i = 0; i < ixn->index.keyPattern.nFields(); ++i) {
            indexKeyBitset.set(i);
        }
    } else if (reqs.getIndexKeyBitset()) {
        indexKeyBitset = *reqs.getIndexKeyBitset();
    }

    auto [stage, outputs] = generateIndexScan(_opCtx,
                                              _collection,
                                              ixn,
                                              indexKeyBitset,
                                              &_slotIdGenerator,
                                              &_frameIdGenerator,
                                              &_spoolIdGenerator,
                                              _yieldPolicy,
                                              _data.env,
                                              _lockAcquisitionCallback);

    if (reqs.has(PlanStageSlots::kReturnKey)) {
        std::vector<std::unique_ptr<sbe::EExpression>> mkObjArgs;

        size_t i = 0;
        for (auto&& elem : ixn->index.keyPattern) {
            mkObjArgs.emplace_back(sbe::makeE<sbe::EConstant>(elem.fieldNameStringData()));
            mkObjArgs.emplace_back(sbe::makeE<sbe::EVariable>((*outputs.getIndexKeySlots())[i++]));
        }

        auto rawKeyExpr = sbe::makeE<sbe::EFunction>("newObj", std::move(mkObjArgs));
        outputs.set(PlanStageSlots::kReturnKey, _slotIdGenerator.generate());
        stage = sbe::makeProjectStage(std::move(stage),
                                      ixn->nodeId(),
                                      outputs.get(PlanStageSlots::kReturnKey),
                                      std::move(rawKeyExpr));
    }

    if (reqs.has(PlanStageSlots::kResult)) {
        outputs.set(PlanStageSlots::kResult, _slotIdGenerator.generate());
        stage = rehydrateIndexKey(std::move(stage),
                                  ixn->index.keyPattern,
                                  ixn->nodeId(),
                                  *outputs.getIndexKeySlots(),
                                  outputs.get(PlanStageSlots::kResult));
    }

    if (reqs.getIndexKeyBitset()) {
        outputs.setIndexKeySlots(
            makeIndexKeyOutputSlotsMatchingParentReqs(ixn->index.keyPattern,
                                                      *reqs.getIndexKeyBitset(),
                                                      indexKeyBitset,
                                                      *outputs.getIndexKeySlots()));
    } else {
        outputs.setIndexKeySlots(boost::none);
    }

    return {std::move(stage), std::move(outputs)};
}

std::tuple<sbe::value::SlotId, sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>>
SlotBasedStageBuilder::makeLoopJoinForFetch(std::unique_ptr<sbe::PlanStage> inputStage,
                                            sbe::value::SlotId seekKeySlot,
                                            PlanNodeId planNodeId,
                                            sbe::value::SlotVector slotsToForward) {
    auto resultSlot = _slotIdGenerator.generate();
    auto recordIdSlot = _slotIdGenerator.generate();

    // Scan the collection in the range [seekKeySlot, Inf).
    auto scanStage = sbe::makeS<sbe::ScanStage>(_collection->uuid(),
                                                resultSlot,
                                                recordIdSlot,
                                                std::vector<std::string>{},
                                                sbe::makeSV(),
                                                seekKeySlot,
                                                true,
                                                nullptr,
                                                planNodeId,
                                                _lockAcquisitionCallback);

    // Get the recordIdSlot from the outer side (e.g., IXSCAN) and feed it to the inner side,
    // limiting the result set to 1 row.
    auto stage = sbe::makeS<sbe::LoopJoinStage>(
        std::move(inputStage),
        sbe::makeS<sbe::LimitSkipStage>(std::move(scanStage), 1, boost::none, planNodeId),
        std::move(slotsToForward),
        sbe::makeSV(seekKeySlot),
        nullptr,
        planNodeId);

    return {resultSlot, recordIdSlot, std::move(stage)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildFetch(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto fn = static_cast<const FetchNode*>(root);

    // At present, makeLoopJoinForFetch() doesn't have the necessary logic for producing an
    // oplogTsSlot, so assert that the caller doesn't need oplogTsSlot.
    invariant(!reqs.has(kOplogTs));

    // The child must produce all of the slots required by the parent of this FetchNode, except for
    // 'resultSlot' which will be produced by the call to makeLoopJoinForFetch() below. In addition
    // to that, the child must always produce a 'recordIdSlot' because it's needed for the call to
    // makeLoopJoinForFetch() below.
    auto childReqs = reqs.copy().clear(kResult).set(kRecordId);

    auto [stage, outputs] = build(fn->children[0], childReqs);

    uassert(4822880, "RecordId slot is not defined", outputs.has(kRecordId));
    uassert(
        4953600, "ReturnKey slot is not defined", !reqs.has(kReturnKey) || outputs.has(kReturnKey));

    auto forwardingReqs = reqs.copy().clear(kResult).clear(kRecordId);

    auto relevantSlots = sbe::makeSV();
    outputs.forEachSlot(forwardingReqs, [&](auto&& slot) { relevantSlots.push_back(slot); });

    // Forward slots for components of the index key if our parent requested them.
    if (auto indexKeySlots = outputs.getIndexKeySlots()) {
        relevantSlots.insert(relevantSlots.end(), indexKeySlots->begin(), indexKeySlots->end());
    }

    sbe::value::SlotId fetchResultSlot, fetchRecordIdSlot;
    std::tie(fetchResultSlot, fetchRecordIdSlot, stage) = makeLoopJoinForFetch(
        std::move(stage), outputs.get(kRecordId), root->nodeId(), std::move(relevantSlots));

    outputs.set(kResult, fetchResultSlot);
    outputs.set(kRecordId, fetchRecordIdSlot);

    if (fn->filter) {
        forwardingReqs = reqs.copy().set(kResult).set(kRecordId);

        relevantSlots = sbe::makeSV();
        outputs.forEachSlot(forwardingReqs, [&](auto&& slot) { relevantSlots.push_back(slot); });

        // Forward slots for components of the index key if our parent requested them.
        if (auto indexKeySlots = outputs.getIndexKeySlots()) {
            relevantSlots.insert(relevantSlots.end(), indexKeySlots->begin(), indexKeySlots->end());
        }

        std::tie(std::ignore, stage) = generateFilter(_opCtx,
                                                      fn->filter.get(),
                                                      std::move(stage),
                                                      &_slotIdGenerator,
                                                      &_frameIdGenerator,
                                                      outputs.get(kResult),
                                                      _data.env,
                                                      std::move(relevantSlots),
                                                      root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildLimit(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const auto ln = static_cast<const LimitNode*>(root);
    boost::optional<long long> skip;

    auto [stage, outputs] = [&]() {
        if (ln->children[0]->getType() == StageType::STAGE_SKIP) {
            // If we have both limit and skip stages and the skip stage is beneath the limit, then
            // we can combine these two stages into one.
            const auto sn = static_cast<const SkipNode*>(ln->children[0]);
            skip = sn->skip;
            return build(sn->children[0], reqs);
        } else {
            return build(ln->children[0], reqs);
        }
    }();

    if (!reqs.getIsTailableCollScanResumeBranch()) {
        stage = std::make_unique<sbe::LimitSkipStage>(
            std::move(stage), ln->limit, skip, root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSkip(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const auto sn = static_cast<const SkipNode*>(root);
    auto [stage, outputs] = build(sn->children[0], reqs);

    if (!reqs.getIsTailableCollScanResumeBranch()) {
        stage = std::make_unique<sbe::LimitSkipStage>(
            std::move(stage), boost::none, sn->skip, root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

namespace {
using MakeSortKeyFn =
    std::function<std::unique_ptr<sbe::EExpression>(sbe::value::SlotId inputSlot)>;

/**
 * Given a field path, this function builds a plan stage tree that will produce the corresponding
 * sort key for that path. The 'makeSortKey' parameter is used to apply any transformations to the
 * leaf fields' values that are necessary (for example, calling collComparisonKey()).
 *
 * Note that when 'level' is 0, this function assumes that 'inputSlot' alrady contains the top-level
 * field value from the path, and thus it will forgo generating a call to getField(). When 'level'
 * is 1 or greater, this function will generate a call to getField() to read the field for that
 * level.
 */
std::pair<sbe::value::SlotId, std::unique_ptr<sbe::PlanStage>> generateSortKeyTraversal(
    std::unique_ptr<sbe::PlanStage> inputStage,
    sbe::value::SlotId inputSlot,
    const FieldPath& fp,
    sbe::value::SortDirection direction,
    FieldIndex level,
    PlanNodeId planNodeId,
    sbe::value::SlotIdGenerator* slotIdGenerator,
    const MakeSortKeyFn& makeSortKey) {
    invariant(level < fp.getPathLength());

    const bool isLeafField = (level == fp.getPathLength() - 1u);

    auto [fieldSlot, fromBranch] = [&]() {
        if (level > 0) {
            // Generate a call to getField() to read the field at the current level and bind it to
            // 'fieldSlot'. According to MQL's sorting semantics, if the field doesn't exist we
            // should use Null as the sort key.
            auto getFieldExpr = makeFunction("getField"_sd,
                                             sbe::makeE<sbe::EVariable>(inputSlot),
                                             sbe::makeE<sbe::EConstant>(fp.getFieldName(level)));

            if (isLeafField) {
                // Wrapping the field access with makeFillEmptyNull() is only necessary for the
                // leaf field. For non-leaf fields, if the field doesn't exist then Nothing will
                // propagate through the TraverseStage and afterward it will be converted to Null
                // by a projection (see below).
                getFieldExpr = makeFillEmptyNull(std::move(getFieldExpr));
            }

            auto fieldSlot{slotIdGenerator->generate()};
            return std::make_pair(
                fieldSlot,
                sbe::makeProjectStage(
                    std::move(inputStage), planNodeId, fieldSlot, std::move(getFieldExpr)));
        }

        return std::make_pair(inputSlot, std::move(inputStage));
    }();

    // Generate the 'in' branch for the TraverseStage that we're about to construct.
    auto [innerSlot, innerBranch] = [&, fieldSlot = fieldSlot, &fromBranch = fromBranch]() {
        if (isLeafField) {
            // Base case: Genereate a ProjectStage to evaluate the predicate.
            auto innerSlot{slotIdGenerator->generate()};
            return std::make_pair(innerSlot,
                                  sbe::makeProjectStage(makeLimitCoScanTree(planNodeId),
                                                        planNodeId,
                                                        innerSlot,
                                                        makeSortKey(fieldSlot)));
        } else {
            // Recursive case.
            return generateSortKeyTraversal(makeLimitCoScanTree(planNodeId),
                                            fieldSlot,
                                            fp,
                                            direction,
                                            level + 1,
                                            planNodeId,
                                            slotIdGenerator,
                                            makeSortKey);
        }
    }();

    // Generate the traverse stage for the current nested level. The fold expression uses
    // well-ordered comparison (cmp3w) to produce the minimum element (if 'direction' is
    // Ascending) or the maximum element (if 'direction' is Descending).
    auto traverseSlot{slotIdGenerator->generate()};
    auto outputSlot{slotIdGenerator->generate()};
    auto op = (direction == sbe::value::SortDirection::Ascending) ? sbe::EPrimBinary::less
                                                                  : sbe::EPrimBinary::greater;

    auto outputStage = sbe::makeS<sbe::TraverseStage>(
        std::move(fromBranch),
        std::move(innerBranch),
        fieldSlot,
        traverseSlot,
        innerSlot,
        sbe::makeSV(),
        sbe::makeE<sbe::EIf>(makeBinaryOp(op,
                                          makeBinaryOp(sbe::EPrimBinary::cmp3w,
                                                       makeVariable(innerSlot),
                                                       makeVariable(traverseSlot)),
                                          makeConstant(sbe::value::TypeTags::NumberInt64,
                                                       sbe::value::bitcastFrom<int64_t>(0))),
                             makeVariable(innerSlot),
                             makeVariable(traverseSlot)),
        nullptr,
        planNodeId,
        1);

    // According to MQL's sorting semantics, when a leaf field is an empty array we should use
    // Undefined as the sort key, and when a non-leaf field is an empty array or doesn't exist
    // we should use Null as the sort key.
    return {outputSlot,
            sbe::makeProjectStage(std::move(outputStage),
                                  planNodeId,
                                  outputSlot,
                                  isLeafField ? makeFillEmptyUndefined(makeVariable(traverseSlot))
                                              : makeFillEmptyNull(makeVariable(traverseSlot)))};
}

/**
 * Given a field path, this function will return an expression that will be true if evaluating the
 * field path involves array traversal at any level of the path (including the leaf field).
 */
std::unique_ptr<sbe::EExpression> generateArrayCheckForSortHelper(
    std::unique_ptr<sbe::EExpression> inputExpr,
    const FieldPath& fp,
    FieldIndex level,
    sbe::value::FrameIdGenerator* frameIdGenerator) {
    invariant(level < fp.getPathLength());

    auto fieldExpr = makeFillEmptyNull(makeFunction(
        "getField"_sd, std::move(inputExpr), sbe::makeE<sbe::EConstant>(fp.getFieldName(level))));

    if (level == fp.getPathLength() - 1u) {
        return makeFunction("isArray"_sd, std::move(fieldExpr));
    } else {
        auto frameId = frameIdGenerator->generate();
        return sbe::makeE<sbe::ELocalBind>(
            frameId,
            sbe::makeEs(std::move(fieldExpr)),
            makeBinaryOp(sbe::EPrimBinary::logicOr,
                         makeFunction("isArray"_sd, makeVariable(frameId, 0)),
                         generateArrayCheckForSortHelper(
                             makeVariable(frameId, 0), fp, level + 1, frameIdGenerator)));
    }
}

/**
 * Given a field path and a slot that holds the top-level field's value from that path, this
 * function will return an expression that will be true if evaluating the field path involves array
 * traversal at any level of the path (including the leaf field).
 */
std::unique_ptr<sbe::EExpression> generateArrayCheckForSort(
    sbe::value::SlotId inputSlot,
    const FieldPath& fp,
    sbe::value::FrameIdGenerator* frameIdGenerator) {
    if (fp.getPathLength() == 1) {
        return makeFunction("isArray"_sd, makeVariable(inputSlot));
    } else {
        return makeBinaryOp(
            sbe::EPrimBinary::logicOr,
            makeFunction("isArray"_sd, makeVariable(inputSlot)),
            generateArrayCheckForSortHelper(makeVariable(inputSlot), fp, 1, frameIdGenerator));
    }
}
}  // namespace

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSort(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    invariant(!reqs.getIndexKeyBitset());

    const auto sn = static_cast<const SortNode*>(root);
    auto sortPattern = SortPattern{sn->pattern, _cq.getExpCtx()};

    tassert(5037001,
            "QueryPlannerAnalysis should not produce a SortNode with an empty sort pattern",
            sortPattern.size() > 0);

    // The child must produce all of the slots required by the parent of this SortNode. In addition
    // to that, the child must always produce a 'resultSlot' because it's needed by the sort logic
    // below.
    auto childReqs = reqs.copy().set(kResult);
    auto [inputStage, outputs] = build(sn->children[0], childReqs);

    auto collatorSlot = _data.env->getSlotIfExists("collator"_sd);

    sbe::value::SlotVector orderBy;
    std::vector<sbe::value::SortDirection> direction;
    StringDataSet prefixSet;
    bool hasPartsWithCommonPrefix = false;

    for (const auto& part : sortPattern) {
        // getExecutor() should never call into buildSlotBasedExecutableTree() when the query
        // contains $meta, so this assertion should always be true.
        tassert(5037002, "Sort with $meta is not supported in SBE", part.fieldPath);

        if (!hasPartsWithCommonPrefix) {
            auto [_, prefixWasNotPresent] = prefixSet.insert(part.fieldPath->getFieldName(0));
            hasPartsWithCommonPrefix = !prefixWasNotPresent;
        }

        // Record the direction for this part of the sort pattern
        direction.push_back(part.isAscending ? sbe::value::SortDirection::Ascending
                                             : sbe::value::SortDirection::Descending);
    }

    if (!hasPartsWithCommonPrefix) {
        sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projectMap;

        for (const auto& part : sortPattern) {
            // Get the top-level field for this sort part. If the field doesn't exist, according to
            // MQL's sorting semantics we should use Null.
            auto getFieldExpr = makeFillEmptyNull(
                makeFunction("getField"_sd,
                             makeVariable(outputs.get(kResult)),
                             sbe::makeE<sbe::EConstant>(part.fieldPath->getFieldName(0))));

            auto fieldSlot{_slotIdGenerator.generate()};
            projectMap.emplace(fieldSlot, std::move(getFieldExpr));

            orderBy.push_back(fieldSlot);
        }

        inputStage = sbe::makeS<sbe::ProjectStage>(
            std::move(inputStage), std::move(projectMap), root->nodeId());

        auto failOnParallelArrays = [&]() -> std::unique_ptr<mongo::sbe::EExpression> {
            auto parallelArraysError = sbe::makeE<sbe::EFail>(
                ErrorCodes::BadValue, "cannot sort with keys that are parallel arrays");

            if (sortPattern.size() < 2) {
                // If the sort pattern only has one part, we don't need to generate a "parallel
                // arrays" check.
                return {};
            } else if (sortPattern.size() == 2) {
                // If the sort pattern has two parts, we can generate a simpler expression to
                // perform the "parallel arrays" check.
                auto makeIsNotArrayCheck = [&](sbe::value::SlotId slot, const FieldPath& fp) {
                    return makeNot(generateArrayCheckForSort(slot, fp, &_frameIdGenerator));
                };

                return makeBinaryOp(
                    sbe::EPrimBinary::logicOr,
                    makeIsNotArrayCheck(orderBy[0], *sortPattern[0].fieldPath),
                    makeBinaryOp(sbe::EPrimBinary::logicOr,
                                 makeIsNotArrayCheck(orderBy[1], *sortPattern[1].fieldPath),
                                 std::move(parallelArraysError)));
            } else {
                // If the sort pattern has three or more parts, we generate an expression to
                // perform the "parallel arrays" check that works (and scales well) for an
                // arbitrary number of sort pattern parts.
                auto makeIsArrayCheck = [&](sbe::value::SlotId slot, const FieldPath& fp) {
                    return makeBinaryOp(sbe::EPrimBinary::cmp3w,
                                        generateArrayCheckForSort(slot, fp, &_frameIdGenerator),
                                        makeConstant(sbe::value::TypeTags::Boolean, false));
                };

                auto numArraysExpr = makeIsArrayCheck(orderBy[0], *sortPattern[0].fieldPath);
                for (size_t idx = 1; idx < sortPattern.size(); ++idx) {
                    numArraysExpr =
                        makeBinaryOp(sbe::EPrimBinary::add,
                                     std::move(numArraysExpr),
                                     makeIsArrayCheck(orderBy[idx], *sortPattern[idx].fieldPath));
                }

                return makeBinaryOp(
                    sbe::EPrimBinary::logicOr,
                    makeBinaryOp(sbe::EPrimBinary::lessEq,
                                 std::move(numArraysExpr),
                                 makeConstant(sbe::value::TypeTags::NumberInt32, 1)),
                    std::move(parallelArraysError));
            }
        }();

        if (failOnParallelArrays) {
            inputStage = sbe::makeProjectStage(std::move(inputStage),
                                               root->nodeId(),
                                               _slotIdGenerator.generate(),
                                               std::move(failOnParallelArrays));
        }

        for (size_t idx = 0; idx < orderBy.size(); ++idx) {
            auto makeSortKey = [&](sbe::value::SlotId inputSlot) {
                return !collatorSlot ? makeVariable(inputSlot)
                                     : makeFunction("collComparisonKey"_sd,
                                                    makeVariable(inputSlot),
                                                    makeVariable(*collatorSlot));
            };

            // Call generateSortKeyTraversal() to build a series of TraverseStages that will
            // traverse this part's field path and produce the corresponding sort key. We pass
            // in the 'makeSortKey' lambda, which will be applied on each leaf field's value
            // to apply the current collation (if there is one).
            sbe::value::SlotId sortKeySlot;
            std::tie(sortKeySlot, inputStage) =
                generateSortKeyTraversal(std::move(inputStage),
                                         orderBy[idx],
                                         *sortPattern[idx].fieldPath,
                                         direction[idx],
                                         0,
                                         root->nodeId(),
                                         &_slotIdGenerator,
                                         makeSortKey);

            orderBy[idx] = sortKeySlot;
        }
    } else {
        // Handle the case where two or more parts of the sort pattern have a common prefix.
        orderBy = _slotIdGenerator.generateMultiple(1);
        direction = {sbe::value::SortDirection::Ascending};

        auto sortSpecExpr =
            makeConstant(sbe::value::TypeTags::sortSpec,
                         sbe::value::bitcastFrom<sbe::value::SortSpec*>(
                             new sbe::value::SortSpec(sn->pattern, _cq.getCollator())));

        inputStage = sbe::makeProjectStage(std::move(inputStage),
                                           root->nodeId(),
                                           orderBy[0],
                                           makeFunction("generateSortKey",
                                                        std::move(sortSpecExpr),
                                                        makeVariable(outputs.get(kResult))));
    }

    auto values = sbe::makeSV();
    outputs.forEachSlot(childReqs, [&](auto&& slot) { values.push_back(slot); });

    inputStage =
        sbe::makeS<sbe::SortStage>(std::move(inputStage),
                                   std::move(orderBy),
                                   std::move(direction),
                                   std::move(values),
                                   sn->limit ? sn->limit : std::numeric_limits<std::size_t>::max(),
                                   sn->maxMemoryUsageBytes,
                                   _cq.getExpCtx()->allowDiskUse,
                                   root->nodeId());

    return {std::move(inputStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildSortKeyGeneraror(const QuerySolutionNode* root,
                                             const PlanStageReqs& reqs) {
    uasserted(4822883, "Sort key generator in not supported in SBE yet");
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildSortMerge(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    using namespace std::literals;
    invariant(!reqs.getIndexKeyBitset());

    auto mergeSortNode = static_cast<const MergeSortNode*>(root);

    const auto sortPattern = SortPattern{mergeSortNode->sort, _cq.getExpCtx()};
    std::vector<sbe::value::SortDirection> direction;

    for (const auto& part : sortPattern) {
        uassert(4822881, "Sorting by expression not supported", !part.expression);
        direction.push_back(part.isAscending ? sbe::value::SortDirection::Ascending
                                             : sbe::value::SortDirection::Descending);
    }

    std::vector<std::unique_ptr<sbe::PlanStage>> inputStages;
    std::vector<sbe::value::SlotVector> inputKeys;
    std::vector<sbe::value::SlotVector> inputVals;

    // Children must produce all of the slots required by the parent of this SortMergeNode. In
    // addition, children must always produce a 'recordIdSlot' if the 'dedup' flag is true.
    auto childReqs = reqs.copy().setIf(kRecordId, mergeSortNode->dedup);

    for (auto&& child : mergeSortNode->children) {
        sbe::value::SlotVector inputKeysForChild;

        // Map of field name to position within the index key. This is used to account for
        // mismatches between the sort pattern and the index key pattern. For instance, suppose the
        // requested sort is {a: 1, b: 1} and the index key pattern is {c: 1, b: 1, a: 1}. When the
        // slots for the relevant components of the index key are generated (i.e. extract keys for
        // 'b' and 'a'),  we wish to insert them into 'inputKeys' in the order that they appear in
        // the sort pattern.
        StringMap<size_t> indexKeyPositionMap;
        auto ixnNode = getNodeByType(child, STAGE_IXSCAN);
        tassert(5184300,
                str::stream() << "Can't build exec tree for node: " << child->toString(),
                ixnNode);

        auto ixn = static_cast<const IndexScanNode*>(ixnNode);
        sbe::IndexKeysInclusionSet indexKeyBitset;
        size_t i = 0;
        for (auto&& elt : ixn->index.keyPattern) {
            for (auto&& sortPart : sortPattern) {
                auto path = sortPart.fieldPath->fullPath();
                if (elt.fieldNameStringData() == path) {
                    indexKeyBitset.set(i);
                    indexKeyPositionMap.emplace(path, indexKeyPositionMap.size());
                    break;
                }
            }
            ++i;
        }
        childReqs.getIndexKeyBitset() = indexKeyBitset;

        // Children must produce a 'resultSlot' if they produce fetched results.
        auto [stage, outputs] = build(child, childReqs);

        tassert(5184301,
                "SORT_MERGE node must receive a RecordID slot as input from child stage"
                " if the 'dedup' flag is set",
                !mergeSortNode->dedup || outputs.has(kRecordId));

        // Clear the index key bitset after building the child stage.
        childReqs.getIndexKeyBitset() = boost::none;

        // Insert the index key slots in the order of the sort pattern.
        auto indexKeys = outputs.extractIndexKeySlots();
        tassert(5184302,
                "SORT_MERGE must receive index key slots as input from its child stages",
                indexKeys);

        for (const auto& part : sortPattern) {
            auto partPath = part.fieldPath->fullPath();
            auto index = indexKeyPositionMap.find(partPath);
            tassert(5184303,
                    str::stream() << "Could not find index key position for sort key part "
                                  << partPath,
                    index != indexKeyPositionMap.end());
            auto indexPos = index->second;
            tassert(5184304,
                    str::stream() << "Index position " << indexPos
                                  << " is not less than number of index components "
                                  << indexKeys->size(),
                    indexPos < indexKeys->size());
            auto indexKeyPart = indexKeys->at(indexPos);
            inputKeysForChild.push_back(indexKeyPart);
        }

        inputKeys.push_back(std::move(inputKeysForChild));
        inputStages.push_back(std::move(stage));

        auto sv = sbe::makeSV();
        outputs.forEachSlot(childReqs, [&](auto&& slot) { sv.push_back(slot); });

        inputVals.push_back(std::move(sv));
    }

    auto outputVals = sbe::makeSV();

    PlanStageSlots outputs(childReqs, &_slotIdGenerator);
    outputs.forEachSlot(childReqs, [&](auto&& slot) { outputVals.push_back(slot); });

    auto stage = sbe::makeS<sbe::SortedMergeStage>(std::move(inputStages),
                                                   std::move(inputKeys),
                                                   std::move(direction),
                                                   std::move(inputVals),
                                                   std::move(outputVals),
                                                   root->nodeId());

    if (mergeSortNode->dedup) {
        stage = sbe::makeS<sbe::UniqueStage>(
            std::move(stage), sbe::makeSV(outputs.get(kRecordId)), root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildProjectionSimple(const QuerySolutionNode* root,
                                             const PlanStageReqs& reqs) {
    using namespace std::literals;
    invariant(!reqs.getIndexKeyBitset());

    auto pn = static_cast<const ProjectionNodeSimple*>(root);

    // The child must produce all of the slots required by the parent of this ProjectionNodeSimple.
    // In addition to that, the child must always produce a 'resultSlot' because it's needed by the
    // projection logic below.
    auto childReqs = reqs.copy().set(kResult);
    auto [inputStage, outputs] = build(pn->children[0], childReqs);

    const auto childResult = outputs.get(kResult);

    outputs.set(kResult, _slotIdGenerator.generate());
    inputStage = sbe::makeS<sbe::MakeBsonObjStage>(std::move(inputStage),
                                                   outputs.get(kResult),
                                                   childResult,
                                                   sbe::MakeBsonObjStage::FieldBehavior::keep,
                                                   pn->proj.getRequiredFields(),
                                                   std::vector<std::string>{},
                                                   sbe::value::SlotVector{},
                                                   true,
                                                   false,
                                                   root->nodeId());

    return {std::move(inputStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildProjectionCovered(const QuerySolutionNode* root,
                                              const PlanStageReqs& reqs) {
    using namespace std::literals;
    invariant(!reqs.getIndexKeyBitset());

    auto pn = static_cast<const ProjectionNodeCovered*>(root);
    invariant(pn->proj.isSimple());

    tassert(5037301,
            str::stream() << "Can't build covered projection for fetched sub-plan: "
                          << root->toString(),
            !pn->children[0]->fetched());

    // This is a ProjectionCoveredNode, so we will be pulling all the data we need from one index.
    // Prepare a bitset to indicate which parts of the index key we need for the projection.
    StringSet requiredFields = {pn->proj.getRequiredFields().begin(),
                                pn->proj.getRequiredFields().end()};

    // The child must produce all of the slots required by the parent of this ProjectionNodeSimple,
    // except for 'resultSlot' which will be produced by the MakeBsonObjStage below. In addition to
    // that, the child must produce the index key slots that are needed by this covered projection.
    //
    // pn->coveredKeyObj is the "index.keyPattern" from the child (which is either an IndexScanNode
    // or DistinctNode). pn->coveredKeyObj lists all the fields that the index can provide, not the
    // fields that the projection wants. requiredFields lists all of the fields that the projection
    // needs. Since this is a covered projection, we're guaranteed that pn->coveredKeyObj contains
    // all of the fields that the projection needs.
    auto childReqs = reqs.copy().clear(kResult);

    auto [indexKeyBitset, keyFieldNames] =
        makeIndexKeyInclusionSet(pn->coveredKeyObj, requiredFields);
    childReqs.getIndexKeyBitset() = std::move(indexKeyBitset);

    auto [inputStage, outputs] = build(pn->children[0], childReqs);

    // Assert that the index scan produced index key slots for this covered projection.
    auto indexKeySlots = *outputs.extractIndexKeySlots();

    outputs.set(kResult, _slotIdGenerator.generate());
    inputStage = sbe::makeS<sbe::MakeBsonObjStage>(std::move(inputStage),
                                                   outputs.get(kResult),
                                                   boost::none,
                                                   boost::none,
                                                   std::vector<std::string>{},
                                                   std::move(keyFieldNames),
                                                   std::move(indexKeySlots),
                                                   true,
                                                   false,
                                                   root->nodeId());

    return {std::move(inputStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildProjectionDefault(const QuerySolutionNode* root,
                                              const PlanStageReqs& reqs) {
    using namespace std::literals;
    invariant(!reqs.getIndexKeyBitset());

    auto pn = static_cast<const ProjectionNodeDefault*>(root);

    // The child must produce all of the slots required by the parent of this ProjectionNodeDefault.
    // In addition to that, the child must always produce a 'resultSlot' because it's needed by the
    // projection logic below.
    auto childReqs = reqs.copy().set(kResult);
    auto [inputStage, outputs] = build(pn->children[0], childReqs);

    auto relevantSlots = sbe::makeSV();
    outputs.forEachSlot(reqs, [&](auto&& slot) { relevantSlots.push_back(slot); });

    auto [slot, stage] = generateProjection(_opCtx,
                                            &pn->proj,
                                            std::move(inputStage),
                                            &_slotIdGenerator,
                                            &_frameIdGenerator,
                                            outputs.get(kResult),
                                            _data.env,
                                            std::move(relevantSlots),
                                            root->nodeId());
    outputs.set(kResult, slot);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildOr(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    invariant(!reqs.getIndexKeyBitset());

    std::vector<std::unique_ptr<sbe::PlanStage>> inputStages;
    std::vector<sbe::value::SlotVector> inputSlots;

    auto orn = static_cast<const OrNode*>(root);

    // Children must produce all of the slots required by the parent of this OrNode. In addition
    // to that, children must always produce a 'recordIdSlot' if the 'dedup' flag is true, and
    // children must always produce a 'resultSlot' if 'filter' is non-null.
    auto childReqs = reqs.copy().setIf(kResult, orn->filter.get()).setIf(kRecordId, orn->dedup);

    for (auto&& child : orn->children) {
        auto [stage, outputs] = build(child, childReqs);

        auto sv = sbe::makeSV();
        outputs.forEachSlot(childReqs, [&](auto&& slot) { sv.push_back(slot); });

        inputStages.push_back(std::move(stage));
        inputSlots.emplace_back(std::move(sv));
    }

    // Construct a union stage whose branches are translated children of the 'Or' node.
    auto unionOutputSlots = sbe::makeSV();

    PlanStageSlots outputs(childReqs, &_slotIdGenerator);
    outputs.forEachSlot(childReqs, [&](auto&& slot) { unionOutputSlots.push_back(slot); });

    auto stage = sbe::makeS<sbe::UnionStage>(
        std::move(inputStages), std::move(inputSlots), std::move(unionOutputSlots), root->nodeId());

    if (orn->dedup) {
        stage = sbe::makeS<sbe::UniqueStage>(
            std::move(stage), sbe::makeSV(outputs.get(kRecordId)), root->nodeId());
    }

    if (orn->filter) {
        auto relevantSlots = sbe::makeSV(outputs.get(kResult));

        auto forwardingReqs = reqs.copy().clear(kResult);
        outputs.forEachSlot(forwardingReqs, [&](auto&& slot) { relevantSlots.push_back(slot); });

        std::tie(std::ignore, stage) = generateFilter(_opCtx,
                                                      orn->filter.get(),
                                                      std::move(stage),
                                                      &_slotIdGenerator,
                                                      &_frameIdGenerator,
                                                      outputs.get(kResult),
                                                      _data.env,
                                                      std::move(relevantSlots),
                                                      root->nodeId());
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildTextMatch(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    tassert(5432212, "no collection object", _collection);
    tassert(5432213, "index keys requsted for text match node", !reqs.getIndexKeyBitset());
    tassert(5432214, "oplogTs requsted for text match node", !reqs.has(kOplogTs));
    tassert(5432215,
            str::stream() << "text match node must have one child, but got "
                          << root->children.size(),
            root->children.size() == 1);
    // TextMatchNode guarantees to produce a fetched sub-plan, but it doesn't fetch itself. Instead,
    // its child sub-plan must be fully fetched, and a text match plan is constructed under this
    // assumption.
    tassert(5432216, "text match input must be fetched", root->children[0]->fetched());

    auto textNode = static_cast<const TextMatchNode*>(root);

    auto childReqs = reqs.copy().set(kResult);
    auto [stage, outputs] = build(textNode->children[0], childReqs);
    tassert(5432217, "result slot is not produced by text match sub-plan", outputs.has(kResult));

    // Create an FTS 'matcher' to apply 'ftsQuery' to matching documents.
    auto matcher = makeFtsMatcher(
        _opCtx, _collection, textNode->index.identifier.catalogName, textNode->ftsQuery.get());

    // Build an 'ftsMatch' expression to match a document stored in the 'kResult' slot using the
    // 'matcher' instance.
    auto ftsMatch =
        makeFunction("ftsMatch",
                     makeConstant(sbe::value::TypeTags::ftsMatcher,
                                  sbe::value::bitcastFrom<fts::FTSMatcher*>(matcher.release())),
                     makeVariable(outputs.get(kResult)));

    // Wrap the 'ftsMatch' expression into an 'if' expression to ensure that it can be applied only
    // to a document.
    auto filter =
        sbe::makeE<sbe::EIf>(makeFunction("isObject", makeVariable(outputs.get(kResult))),
                             std::move(ftsMatch),
                             sbe::makeE<sbe::EFail>(ErrorCodes::Error{4623400},
                                                    "textmatch requires input to be an object"));

    // Add a filter stage to apply 'ftsQuery' to matching documents and discard documents which do
    // not match.
    stage =
        sbe::makeS<sbe::FilterStage<false>>(std::move(stage), std::move(filter), root->nodeId());

    if (reqs.has(kReturnKey)) {
        // Assign the 'returnKeySlot' to be the empty object.
        outputs.set(kReturnKey, _slotIdGenerator.generate());
        stage = sbe::makeProjectStage(
            std::move(stage), root->nodeId(), outputs.get(kReturnKey), makeFunction("newObj"));
    }

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildReturnKey(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    invariant(!reqs.getIndexKeyBitset());

    // TODO SERVER-49509: If the projection includes {$meta: "sortKey"}, the result of this stage
    // should also include the sort key. Everything else in the projection is ignored.
    auto returnKeyNode = static_cast<const ReturnKeyNode*>(root);

    // The child must produce all of the slots required by the parent of this ReturnKeyNode except
    // for 'resultSlot'. In addition to that, the child must always produce a 'returnKeySlot'.
    // After build() returns, we take the 'returnKeySlot' produced by the child and store it into
    // 'resultSlot' for the parent of this ReturnKeyNode to consume.
    auto childReqs = reqs.copy().clear(kResult).set(kReturnKey);
    auto [stage, outputs] = build(returnKeyNode->children[0], childReqs);

    outputs.set(kResult, outputs.get(kReturnKey));
    outputs.clear(kReturnKey);

    return {std::move(stage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildEof(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    return generateEofPlan(root->nodeId(), reqs, &_slotIdGenerator);
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildAndHash(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto andHashNode = static_cast<const AndHashNode*>(root);

    tassert(5073711, "need at least two children for AND_HASH", andHashNode->children.size() >= 2);

    auto childReqs = reqs.copy().set(kResult).set(kRecordId);

    auto outerChild = andHashNode->children[0];
    auto innerChild = andHashNode->children[1];

    auto [outerStage, outerOutputs] = build(outerChild, childReqs);
    auto outerIdSlot = outerOutputs.get(kRecordId);
    auto outerResultSlot = outerOutputs.get(kResult);
    auto outerCondSlots = sbe::makeSV(outerIdSlot);
    auto outerProjectSlots = sbe::makeSV(outerResultSlot);

    auto [innerStage, innerOutputs] = build(innerChild, childReqs);
    tassert(5073712, "innerOutputs must contain kRecordId slot", innerOutputs.has(kRecordId));
    tassert(5073713, "innerOutputs must contain kResult slot", innerOutputs.has(kResult));
    auto innerIdSlot = innerOutputs.get(kRecordId);
    auto innerResultSlot = innerOutputs.get(kResult);
    auto innerCondSlots = sbe::makeSV(innerIdSlot);
    auto innerProjectSlots = sbe::makeSV(innerResultSlot);

    auto collatorSlot = _data.env->getSlotIfExists("collator"_sd);

    // Designate outputs.
    PlanStageSlots outputs(reqs, &_slotIdGenerator);
    if (reqs.has(kRecordId)) {
        outputs.set(kRecordId, innerIdSlot);
    }
    if (reqs.has(kResult)) {
        outputs.set(kResult, innerResultSlot);
    }

    auto hashJoinStage = sbe::makeS<sbe::HashJoinStage>(std::move(outerStage),
                                                        std::move(innerStage),
                                                        outerCondSlots,
                                                        outerProjectSlots,
                                                        innerCondSlots,
                                                        innerProjectSlots,
                                                        collatorSlot,
                                                        root->nodeId());

    // If there are more than 2 children, iterate all remaining children and hash
    // join together.
    for (size_t i = 2; i < andHashNode->children.size(); i++) {
        auto [stage, outputs] = build(andHashNode->children[i], childReqs);
        tassert(5073714, "outputs must contain kRecordId slot", outputs.has(kRecordId));
        tassert(5073715, "outputs must contain kResult slot", outputs.has(kResult));
        auto idSlot = outputs.get(kRecordId);
        auto resultSlot = outputs.get(kResult);
        auto condSlots = sbe::makeSV(idSlot);
        auto projectSlots = sbe::makeSV(resultSlot);

        // The previous HashJoinStage is always set as the inner stage, so that we can reuse the
        // innerIdSlot and innerResultSlot that have been designated as outputs.
        hashJoinStage = sbe::makeS<sbe::HashJoinStage>(std::move(stage),
                                                       std::move(hashJoinStage),
                                                       condSlots,
                                                       projectSlots,
                                                       innerCondSlots,
                                                       innerProjectSlots,
                                                       collatorSlot,
                                                       root->nodeId());
    }

    return {std::move(hashJoinStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildAndSorted(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    auto andSortedNode = static_cast<const AndSortedNode*>(root);

    // Need at least two children.
    tassert(
        5073706, "need at least two children for AND_SORTED", andSortedNode->children.size() >= 2);

    auto childReqs = reqs.copy().set(kResult).set(kRecordId);

    auto outerChild = andSortedNode->children[0];
    auto innerChild = andSortedNode->children[1];

    auto [outerStage, outerOutputs] = build(outerChild, childReqs);
    auto outerIdSlot = outerOutputs.get(kRecordId);
    auto outerResultSlot = outerOutputs.get(kResult);

    auto outerKeySlots = sbe::makeSV(outerIdSlot);
    auto outerProjectSlots = sbe::makeSV(outerResultSlot);

    auto [innerStage, innerOutputs] = build(innerChild, childReqs);
    tassert(5073707, "innerOutputs must contain kRecordId slot", innerOutputs.has(kRecordId));
    tassert(5073708, "innerOutputs must contain kResult slot", innerOutputs.has(kResult));
    auto innerIdSlot = innerOutputs.get(kRecordId);
    auto innerResultSlot = innerOutputs.get(kResult);

    auto innerKeySlots = sbe::makeSV(innerIdSlot);
    auto innerProjectSlots = sbe::makeSV(innerResultSlot);

    PlanStageSlots outputs(reqs, &_slotIdGenerator);
    if (reqs.has(kRecordId)) {
        outputs.set(kRecordId, innerIdSlot);
    }
    if (reqs.has(kResult)) {
        outputs.set(kResult, innerResultSlot);
    }

    std::vector<sbe::value::SortDirection> sortDirs(outerKeySlots.size(),
                                                    sbe::value::SortDirection::Ascending);

    auto mergeJoinStage = sbe::makeS<sbe::MergeJoinStage>(std::move(outerStage),
                                                          std::move(innerStage),
                                                          outerKeySlots,
                                                          outerProjectSlots,
                                                          innerKeySlots,
                                                          innerProjectSlots,
                                                          sortDirs,
                                                          root->nodeId());

    // If there are more than 2 children, iterate all remaining children and merge
    // join together.
    for (size_t i = 2; i < andSortedNode->children.size(); i++) {
        auto [stage, outputs] = build(andSortedNode->children[i], childReqs);
        tassert(5073709, "outputs must contain kRecordId slot", outputs.has(kRecordId));
        tassert(5073710, "outputs must contain kResult slot", outputs.has(kResult));
        auto idSlot = outputs.get(kRecordId);
        auto resultSlot = outputs.get(kResult);
        auto keySlots = sbe::makeSV(idSlot);
        auto projectSlots = sbe::makeSV(resultSlot);

        mergeJoinStage = sbe::makeS<sbe::MergeJoinStage>(std::move(stage),
                                                         std::move(mergeJoinStage),
                                                         keySlots,
                                                         projectSlots,
                                                         innerKeySlots,
                                                         innerProjectSlots,
                                                         sortDirs,
                                                         root->nodeId());
    }

    return {std::move(mergeJoinStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::makeUnionForTailableCollScan(const QuerySolutionNode* root,
                                                    const PlanStageReqs& reqs) {
    using namespace std::literals;
    invariant(!reqs.getIndexKeyBitset());

    // Register a SlotId in the global environment which would contain a recordId to resume a
    // tailable collection scan from. A PlanStage executor will track the last seen recordId and
    // will reset a SlotAccessor for the resumeRecordIdSlot with this recordId.
    auto resumeRecordIdSlot = _data.env->registerSlot(
        "resumeRecordId"_sd, sbe::value::TypeTags::Nothing, 0, false, &_slotIdGenerator);

    // For tailable collection scan we need to build a special union sub-tree consisting of two
    // branches:
    //   1) An anchor branch implementing an initial collection scan before the first EOF is hit.
    //   2) A resume branch implementing all consecutive collection scans from a recordId which was
    //      seen last.
    //
    // The 'makeStage' parameter is used to build a PlanStage tree which is served as a root stage
    // for each of the union branches. The same machanism is used to build each union branch, and
    // the special logic which needs to be triggered depending on which branch we build is
    // controlled by setting the isTailableCollScanResumeBranch flag in PlanStageReqs.
    auto makeUnionBranch = [&](bool isTailableCollScanResumeBranch)
        -> std::pair<sbe::value::SlotVector, std::unique_ptr<sbe::PlanStage>> {
        auto childReqs = reqs;
        childReqs.setIsTailableCollScanResumeBranch(isTailableCollScanResumeBranch);
        auto [branch, outputs] = build(root, childReqs);

        auto branchSlots = sbe::makeSV();
        outputs.forEachSlot(reqs, [&](auto&& slot) { branchSlots.push_back(slot); });

        return {std::move(branchSlots), std::move(branch)};
    };

    // Build an anchor branch of the union and add a constant filter on top of it, so that it would
    // only execute on an initial collection scan, that is, when resumeRecordId is not available
    // yet.
    auto&& [anchorBranchSlots, anchorBranch] = makeUnionBranch(false);
    anchorBranch = sbe::makeS<sbe::FilterStage<true>>(
        std::move(anchorBranch),
        makeNot(makeFunction("exists"_sd, sbe::makeE<sbe::EVariable>(resumeRecordIdSlot))),
        root->nodeId());

    // Build a resume branch of the union and add a constant filter on op of it, so that it would
    // only execute when we resume a collection scan from the resumeRecordId.
    auto&& [resumeBranchSlots, resumeBranch] = makeUnionBranch(true);
    resumeBranch = sbe::makeS<sbe::FilterStage<true>>(
        sbe::makeS<sbe::LimitSkipStage>(std::move(resumeBranch), boost::none, 1, root->nodeId()),
        sbe::makeE<sbe::EFunction>("exists"_sd,
                                   sbe::makeEs(sbe::makeE<sbe::EVariable>(resumeRecordIdSlot))),
        root->nodeId());

    invariant(anchorBranchSlots.size() == resumeBranchSlots.size());

    // A vector of the output slots for each union branch.
    auto branchSlots = makeVector<sbe::value::SlotVector>(std::move(anchorBranchSlots),
                                                          std::move(resumeBranchSlots));

    auto unionOutputSlots = sbe::makeSV();

    PlanStageSlots outputs(reqs, &_slotIdGenerator);
    outputs.forEachSlot(reqs, [&](auto&& slot) { unionOutputSlots.push_back(slot); });

    // Branch output slots become the input slots to the union.
    auto unionStage =
        sbe::makeS<sbe::UnionStage>(makeVector<std::unique_ptr<sbe::PlanStage>>(
                                        std::move(anchorBranch), std::move(resumeBranch)),
                                    branchSlots,
                                    unionOutputSlots,
                                    root->nodeId());

    return {std::move(unionStage), std::move(outputs)};
}

namespace {
/**
 * Given an SBE subtree 'childStage' which computes the shard key and puts it into the given
 * 'shardKeySlot', augments the SBE plan to actually perform shard filtering. Namely, a FilterStage
 * is added at the root of the tree whose filter expression uses 'shardFilterer' to determine
 * whether the shard key value in 'shardKeySlot' belongs to an owned range or not.
 */
auto buildShardFilterGivenShardKeySlot(sbe::value::SlotId shardKeySlot,
                                       std::unique_ptr<sbe::PlanStage> childStage,
                                       std::unique_ptr<ShardFilterer> shardFilterer,
                                       PlanNodeId nodeId) {
    auto shardFilterFn =
        makeFunction("shardFilter",
                     makeConstant(sbe::value::TypeTags::shardFilterer,
                                  sbe::value::bitcastFrom<ShardFilterer*>(shardFilterer.release())),
                     sbe::makeE<sbe::EVariable>(shardKeySlot));

    return sbe::makeS<sbe::FilterStage<false>>(
        std::move(childStage), std::move(shardFilterFn), nodeId);
}
}  // namespace

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>
SlotBasedStageBuilder::buildShardFilterCovered(const ShardingFilterNode* filterNode,
                                               std::unique_ptr<ShardFilterer> shardFilterer,
                                               BSONObj shardKeyPattern,
                                               BSONObj indexKeyPattern,
                                               const QuerySolutionNode* child,
                                               PlanStageReqs childReqs) {
    StringDataSet shardKeyFields;
    for (auto&& shardKeyElt : shardKeyPattern) {
        shardKeyFields.insert(shardKeyElt.fieldNameStringData());
    }

    // Save the bit vector describing the fields from the index that our parent requires. The shard
    // filtering process may require additional fields that are not needed by the parent (for
    // example, if the parent is projecting field "a" but the shard key is {a: 1, b: 1}). We will
    // need the parent's reqs later on so that we can hand the correct slot vector for these fields
    // back to our parent.
    auto parentIndexKeyReqs = childReqs.getIndexKeyBitset();

    // Determine the set of fields from the index required to obtain the shard key and union those
    // with the set of fields from the index required by the parent stage.
    auto [shardKeyIndexReqs, projectFields] =
        makeIndexKeyInclusionSet(indexKeyPattern, shardKeyFields);
    childReqs.getIndexKeyBitset() =
        parentIndexKeyReqs.value_or(sbe::IndexKeysInclusionSet{}) | shardKeyIndexReqs;

    auto [stage, outputs] = build(child, childReqs);

    invariant(outputs.getIndexKeySlots());
    auto indexKeySlots = *outputs.getIndexKeySlots();

    auto shardKeySlot = _slotIdGenerator.generate();

    auto mkObjStage = sbe::makeS<sbe::MakeBsonObjStage>(std::move(stage),
                                                        shardKeySlot,
                                                        boost::none,
                                                        boost::none,
                                                        std::vector<std::string>{},
                                                        std::move(projectFields),
                                                        indexKeySlots,
                                                        true,
                                                        false,
                                                        filterNode->nodeId());

    auto filterStage = buildShardFilterGivenShardKeySlot(
        shardKeySlot, std::move(mkObjStage), std::move(shardFilterer), filterNode->nodeId());

    outputs.setIndexKeySlots(!parentIndexKeyReqs ? boost::none
                                                 : boost::optional<sbe::value::SlotVector>{
                                                       makeIndexKeyOutputSlotsMatchingParentReqs(
                                                           indexKeyPattern,
                                                           *parentIndexKeyReqs,
                                                           *childReqs.getIndexKeyBitset(),
                                                           indexKeySlots)});

    return {std::move(filterStage), std::move(outputs)};
}

std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::buildShardFilter(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    const auto filterNode = static_cast<const ShardingFilterNode*>(root);

    // If we're sharded make sure that we don't return data that isn't owned by the shard. This
    // situation can occur when pending documents from in-progress migrations are inserted and when
    // there are orphaned documents from aborted migrations. To check if the document is owned by
    // the shard, we need to own a 'ShardFilterer', and extract the document's shard key as a
    // BSONObj.
    auto shardFilterer = _shardFiltererFactory->makeShardFilterer(_opCtx);
    auto shardKeyPattern = shardFilterer->getKeyPattern().toBSON();

    // Determine if our child is an index scan and extract it's key pattern, or empty BSONObj if our
    // child is not an IXSCAN node.
    BSONObj indexKeyPattern = [&]() {
        auto childNode = filterNode->children[0];
        switch (childNode->getType()) {
            case StageType::STAGE_IXSCAN:
                return static_cast<const IndexScanNode*>(childNode)->index.keyPattern;
            case StageType::STAGE_VIRTUAL_SCAN:
                return static_cast<const VirtualScanNode*>(childNode)->indexKeyPattern;
            default:
                return BSONObj{};
        }
    }();

    // If we're not required to fill out the 'kResult' slot, then instead we can request a slot from
    // the child for each of the fields which constitute the shard key. This allows us to avoid
    // materializing an intermediate object for plans where shard filtering can be performed based
    // on the contents of index keys.
    //
    // We only apply this optimization in the special case that the child QSN is an IXSCAN, since in
    // this case we can request exactly the fields we need according to their position in the index
    // key pattern.
    auto childReqs = reqs.copy().setIf(kResult, indexKeyPattern.isEmpty());
    if (!childReqs.has(kResult)) {
        return buildShardFilterCovered(filterNode,
                                       std::move(shardFilterer),
                                       std::move(shardKeyPattern),
                                       std::move(indexKeyPattern),
                                       filterNode->children[0],
                                       std::move(childReqs));
    }

    auto [stage, outputs] = build(filterNode->children[0], childReqs);

    // Build an expression to extract the shard key from the document based on the shard key
    // pattern. To do this, we iterate over the shard key pattern parts and build nested 'getField'
    // expressions. This will handle single-element paths, and dotted paths for each shard key part.
    sbe::value::SlotMap<std::unique_ptr<sbe::EExpression>> projections;
    sbe::value::SlotVector fieldSlots;
    std::vector<std::string> projectFields;
    std::unique_ptr<sbe::EExpression> bindShardKeyPart;

    for (auto&& keyPatternElem : shardKeyPattern) {
        auto fieldRef = FieldRef{keyPatternElem.fieldNameStringData()};
        fieldSlots.push_back(_slotIdGenerator.generate());
        projectFields.push_back(fieldRef.dottedField().toString());

        auto currentFieldSlot = sbe::makeE<sbe::EVariable>(outputs.get(kResult));
        auto shardKeyBinding =
            generateShardKeyBinding(fieldRef, _frameIdGenerator, std::move(currentFieldSlot), 0);

        // If this is a hashed shard key then compute the hash value.
        if (ShardKeyPattern::isHashedPatternEl(keyPatternElem)) {
            shardKeyBinding = makeFunction("shardHash"_sd, std::move(shardKeyBinding));
        }

        projections.emplace(fieldSlots.back(), std::move(shardKeyBinding));
    }

    auto shardKeySlot{_slotIdGenerator.generate()};

    // Build an object which will hold a flattened shard key from the projections above.
    auto shardKeyObjStage = sbe::makeS<sbe::MakeBsonObjStage>(
        sbe::makeS<sbe::ProjectStage>(std::move(stage), std::move(projections), root->nodeId()),
        shardKeySlot,
        boost::none,
        boost::none,
        std::vector<std::string>{},
        projectFields,
        fieldSlots,
        true,
        false,
        root->nodeId());

    // Build a project stage that checks if any of the fieldSlots for the shard key parts are an
    // Array which is represented by Nothing.
    invariant(fieldSlots.size() > 0);
    auto arrayChecks = makeNot(sbe::makeE<sbe::EFunction>(
        "exists", sbe::makeEs(sbe::makeE<sbe::EVariable>(fieldSlots[0]))));
    for (size_t ind = 1; ind < fieldSlots.size(); ++ind) {
        arrayChecks = makeBinaryOp(
            sbe::EPrimBinary::Op::logicOr,
            std::move(arrayChecks),
            makeNot(makeFunction("exists", sbe::makeE<sbe::EVariable>(fieldSlots[ind]))));
    }
    arrayChecks = sbe::makeE<sbe::EIf>(std::move(arrayChecks),
                                       sbe::makeE<sbe::EConstant>(sbe::value::TypeTags::Nothing, 0),
                                       sbe::makeE<sbe::EVariable>(shardKeySlot));

    auto finalShardKeySlot{_slotIdGenerator.generate()};

    auto finalShardKeyObjStage = makeProjectStage(
        std::move(shardKeyObjStage), root->nodeId(), finalShardKeySlot, std::move(arrayChecks));

    return {buildShardFilterGivenShardKeySlot(finalShardKeySlot,
                                              std::move(finalShardKeyObjStage),
                                              std::move(shardFilterer),
                                              root->nodeId()),
            std::move(outputs)};
}

// Returns a non-null pointer to the root of a plan tree, or a non-OK status if the PlanStage tree
// could not be constructed.
std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots> SlotBasedStageBuilder::build(
    const QuerySolutionNode* root, const PlanStageReqs& reqs) {
    static const stdx::unordered_map<
        StageType,
        std::function<std::pair<std::unique_ptr<sbe::PlanStage>, PlanStageSlots>(
            SlotBasedStageBuilder&, const QuerySolutionNode* root, const PlanStageReqs& reqs)>>
        kStageBuilders = {
            {STAGE_COLLSCAN, &SlotBasedStageBuilder::buildCollScan},
            {STAGE_VIRTUAL_SCAN, &SlotBasedStageBuilder::buildVirtualScan},
            {STAGE_IXSCAN, &SlotBasedStageBuilder::buildIndexScan},
            {STAGE_FETCH, &SlotBasedStageBuilder::buildFetch},
            {STAGE_LIMIT, &SlotBasedStageBuilder::buildLimit},
            {STAGE_SKIP, &SlotBasedStageBuilder::buildSkip},
            {STAGE_SORT_SIMPLE, &SlotBasedStageBuilder::buildSort},
            {STAGE_SORT_DEFAULT, &SlotBasedStageBuilder::buildSort},
            {STAGE_SORT_KEY_GENERATOR, &SlotBasedStageBuilder::buildSortKeyGeneraror},
            {STAGE_PROJECTION_SIMPLE, &SlotBasedStageBuilder::buildProjectionSimple},
            {STAGE_PROJECTION_DEFAULT, &SlotBasedStageBuilder::buildProjectionDefault},
            {STAGE_PROJECTION_COVERED, &SlotBasedStageBuilder::buildProjectionCovered},
            {STAGE_OR, &SlotBasedStageBuilder::buildOr},
            // In SBE TEXT_OR behaves like a regular OR. All the work to support "textScore"
            // metadata is done outside of TEXT_OR, unlike the legacy implementation.
            {STAGE_TEXT_OR, &SlotBasedStageBuilder::buildOr},
            {STAGE_TEXT_MATCH, &SlotBasedStageBuilder::buildTextMatch},
            {STAGE_RETURN_KEY, &SlotBasedStageBuilder::buildReturnKey},
            {STAGE_EOF, &SlotBasedStageBuilder::buildEof},
            {STAGE_AND_HASH, &SlotBasedStageBuilder::buildAndHash},
            {STAGE_AND_SORTED, &SlotBasedStageBuilder::buildAndSorted},
            {STAGE_SORT_MERGE, &SlotBasedStageBuilder::buildSortMerge},
            {STAGE_SHARDING_FILTER, &SlotBasedStageBuilder::buildShardFilter}};

    tassert(4822884,
            str::stream() << "Unsupported QSN in SBE stage builder: " << root->toString(),
            kStageBuilders.find(root->getType()) != kStageBuilders.end());

    // If this plan is for a tailable cursor scan, and we're not already in the process of building
    // a special union sub-tree implementing such scans, then start building a union sub-tree. Note
    // that LIMIT or SKIP stage is used as a splitting point of the two union branches, if present,
    // because we need to apply limit (or skip) only in the initial scan (in the anchor branch), and
    // the resume branch should not have it.
    switch (root->getType()) {
        case STAGE_COLLSCAN:
        case STAGE_LIMIT:
        case STAGE_SKIP:
            if (_cq.getFindCommandRequest().getTailable() &&
                !reqs.getIsBuildingUnionForTailableCollScan()) {
                auto childReqs = reqs;
                childReqs.setIsBuildingUnionForTailableCollScan(true);
                return makeUnionForTailableCollScan(root, childReqs);
            }
        default:
            break;
    }

    return std::invoke(kStageBuilders.at(root->getType()), *this, root, reqs);
}
}  // namespace mongo::stage_builder
