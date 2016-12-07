#include <memory>

#include <iostream>
#include <algorithm>
#include <vector>

#include "c_frontend.h"
#include "evaluate.hpp"
#include "backend/MergeFunctions.hpp"
#include "backend/basic_watershed.hpp"
#include "backend/region_graph.hpp"

std::map<int, WaterzContext*> WaterzContext::_contexts;
int WaterzContext::_nextId = 0;

WaterzState
initialize(
		size_t          width,
		size_t          height,
		size_t          depth,
		const AffValue* affinity_data,
		SegID*          segmentation_data,
		const GtID*     ground_truth_data,
		AffValue        affThresholdLow,
		AffValue        affThresholdHigh) {

	size_t num_voxels = width*height*depth;

	// wrap affinities (no copy)
	affinity_graph_ref<AffValue> affinities(
			affinity_data,
			boost::extents[3][width][height][depth]
	);

	// wrap segmentation array (no copy)
	volume_ref_ptr<SegID> segmentation(
			new volume_ref<SegID>(
					segmentation_data,
					boost::extents[width][height][depth]
			)
	);

	std::cout << "performing initial watershed segmentation..." << std::endl;

	counts_t<size_t> counts;
	watershed(affinities, affThresholdLow, affThresholdHigh, *segmentation, counts);

	std::size_t numNodes = counts.size();

	std::cout << "creating region graph for " << numNodes << " nodes" << std::endl;

	std::shared_ptr<RegionGraphType> regionGraph(
			new RegionGraphType(numNodes)
	);

	std::cout << "creating edge affinity map" << std::endl;

	std::shared_ptr<RegionGraphType::EdgeMap<float>> edgeAffinities(
			new RegionGraphType::EdgeMap<float>(*regionGraph)
	);

	std::cout << "creating region size map" << std::endl;

	// create region size node map, desctruct counts
	std::shared_ptr<RegionGraphType::NodeMap<size_t>> regionSizes(
			new RegionGraphType::NodeMap<size_t>(*regionGraph, std::move(counts))
	);

	std::cout << "extracting region graph..." << std::endl;

	get_region_graph(
			affinities,
			*segmentation,
			numNodes - 1,
			*regionGraph,
			*edgeAffinities);

	std::shared_ptr<ScoringFunctionType> scoringFunction(
			new ScoringFunctionType(*edgeAffinities, *regionSizes)
	);

	std::shared_ptr<RegionMergingType> regionMerging(
			new RegionMergingType(*regionGraph)
	);

	WaterzContext* context = WaterzContext::createNew();
	context->regionGraph     = regionGraph;
	context->edgeAffinities  = edgeAffinities;
	context->regionSizes     = regionSizes;
	context->regionMerging   = regionMerging;
	context->scoringFunction = scoringFunction;
	context->segmentation    = segmentation;

	WaterzState initial_state;
	initial_state.context = context->id;

	if (ground_truth_data != NULL) {

		// wrap ground-truth (no copy)
		volume_const_ref_ptr<GtID> groundtruth(
				new volume_const_ref<GtID>(
						ground_truth_data,
						boost::extents[width][height][depth]
				)
		);

		context->groundtruth = groundtruth;
	}

	return initial_state;
}

void
mergeUntil(
		WaterzState& state,
		float        threshold) {

	WaterzContext* context = WaterzContext::get(state.context);

	if (threshold > 0) {

		std::cout << "merging until threshold " << threshold << std::endl;

		context->regionMerging->mergeUntil(
				*context->scoringFunction,
				threshold);

		std::cout << "extracting segmentation" << std::endl;

		context->regionMerging->extractSegmentation(*context->segmentation);
	}

	if (context->groundtruth) {

		std::cout << "evaluating current segmentation against ground-truth" << std::endl;

		auto m = compare_volumes(*context->groundtruth, *context->segmentation);

		state.metrics.rand_split = std::get<0>(m);
		state.metrics.rand_merge = std::get<1>(m);
		state.metrics.voi_split  = std::get<2>(m);
		state.metrics.voi_merge  = std::get<3>(m);
	}
}

void
free(WaterzState& state) {

	WaterzContext::free(state.context);
}