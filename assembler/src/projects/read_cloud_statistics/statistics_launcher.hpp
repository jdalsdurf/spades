#pragma once

#include "scaffolder_validation.hpp"
#include <common/modules/path_extend/scaffolder2015/scaffold_graph_constructor.hpp>
#include "common/assembly_graph/contracted_graph/contracted_graph_builder.hpp"
#include "contracted_graph_stats/contracted_graph_analyzer.hpp"
#include "cluster_storage_analyzer.hpp"
#include "scaffold_graph_utils.hpp"
#include "statistics_processor.hpp"
#include "../../common/modules/path_extend/pipeline/launch_support.hpp"
#include "../../common/assembly_graph/graph_support/scaff_supplementary.hpp"
#include "contig_path_analyzer.hpp"
#include "contracted_graph_stats/contracted_graph_local_statistics.hpp"
#include "scaffolder_statistics/scaffolder_stats.hpp"
#include "common/modules/path_extend/read_cloud_path_extend/scaffold_graph_construction_pipeline.hpp"
#include "common/modules/path_extend/read_cloud_path_extend/validation/scaffold_graph_validation.hpp"
#include "scaffolder_statistics/gap_closer_stats.hpp"
#include "scaffolder_statistics/gap_closer_analyzer.hpp"
#include "path_cluster_test.hpp"
#include "common/modules/path_extend/read_cloud_path_extend/path_cluster_helper.hpp"

namespace read_cloud_statistics {

    struct StatisticsConfigs {
      const size_t edge_read_cluster_threshold;
      const size_t global_read_cluster_threshold;

      StatisticsConfigs(const size_t edge_read_cluster_threshold, const size_t global_read_cluster_threshold)
          : edge_read_cluster_threshold(edge_read_cluster_threshold),
            global_read_cluster_threshold(global_read_cluster_threshold) {}
    };

    class StatisticsLauncher {
     public:
        typedef path_extend::validation::ContigTransitionStorage ContigTransitionStorage;
        typedef path_extend::transitions::Transition Transition;
        typedef path_extend::validation::ContigPathBuilder ContigPathBuilder;
        typedef path_extend::validation::TransitionStorageBuilder TransitionStorageBuilder;
        typedef path_extend::validation::ClusterTransitionExtractor ClusterTransitionExtractor;
        typedef path_extend::validation::ContigPathFilter ContigPathFilter;
     private:
        const debruijn_graph::conj_graph_pack& gp_;
        const path_extend::ScaffoldingUniqueEdgeStorage& unique_storage_;
        shared_ptr<barcode_index::FrameBarcodeIndexInfoExtractor> barcode_extractor_ptr_;
        StatisticsConfigs configs_;
     public:
        StatisticsLauncher(const debruijn_graph::conj_graph_pack& gp,
                           const path_extend::ScaffoldingUniqueEdgeStorage &unique_storage_,
                           shared_ptr<barcode_index::FrameBarcodeIndexInfoExtractor> barcode_extractor_ptr,
                           const StatisticsConfigs &configs_) : gp_(gp), unique_storage_(unique_storage_),
                                                                barcode_extractor_ptr_(barcode_extractor_ptr),
                                                                configs_(configs_) {}

        void Launch(const string& base_output_path) {
            INFO("Transition stats:");
            vector<size_t> distances = {1000, 2500, 5000, 7500, 10000, 20000, 35000, 50000};

            std::function<void(const string&, size_t)> path_cluster_analyzer = [=](const string& path, size_t dist) {
              this->AnalyzePathClusters(path, dist);
            };

            std::function<void(const string&, size_t)> contracted_graph_analyzer = [=](const string& path, size_t dist) {
              this->AnalyzeContractedGraph(path, dist);
            };
            size_t distance = cfg::get().ts_res.distance;

//            AnalyzeScaffoldGapCloser(base_output_path);
            AnalyzeScaffoldGraph(base_output_path);

//            AnalyzeContractedGraph(base_output_path, distance);
//            LaunchAnalyzerForMultipleDistances(base_output_path, contracted_graph_analyzer, distances, cfg::get().max_threads);

//            INFO("Analyzing contig paths");
//            AnalyzeContigPaths(base_output_path);

//            INFO("Analyzing path clusters");
//            LaunchAnalyzerForMultipleDistances(base_output_path, path_cluster_analyzer, distances, cfg::get().max_threads);
//            AnalyzePathClusters(base_output_path, distance);
//
//            AnalyzeTransitions(base_output_path, distance);
//            AnalyzeTransitionsForMultipleDistances(graph_pack, stats_path);
        }

     private:

        ContigTransitionStorage BuildFilteredTransitionStorage(const ContigPathBuilder& contig_path_builder,
                                                               shared_ptr<TransitionStorageBuilder> transition_builder,
                                                               const path_extend::ScaffoldingUniqueEdgeStorage& unique_storage,
                                                               const string& path_to_contigs) {
            const string EMPTY_STRING = "";
            ContigTransitionStorage result;
            if (path_to_contigs != EMPTY_STRING) {
                auto named_reference_paths = contig_path_builder.GetContigPaths(path_to_contigs);
                auto reference_paths = contig_path_builder.StripNames(named_reference_paths);
                ContigPathFilter contig_path_filter(unique_storage);
                result = transition_builder->GetTransitionStorage(contig_path_filter.FilterPathsUsingUniqueStorage(
                    reference_paths));
            }
            INFO("Transition storage size: " << result.size());
            return result;
        }

        std::unordered_map<string, ContigTransitionStorage>
        BuildTransitionStorages(const path_extend::ScaffoldingUniqueEdgeStorage& unique_storage) {
            const string path_to_reference = cfg::get().ts_res.statistics.genome_path;
            const string path_to_base_contigs = cfg::get().ts_res.statistics.base_contigs_path;
            const string path_to_cloud_contigs = cfg::get().ts_res.statistics.cloud_contigs_path;
            const string EMPTY_STRING = "";
            std::unordered_map<string, ContigTransitionStorage> name_to_transition_storage;
            INFO("Reference path: " << path_to_reference);
            INFO("Base contigs path: " << path_to_base_contigs);
            INFO("Cloud contigs path: " << path_to_cloud_contigs);

            ContigPathBuilder contig_path_builder(gp_);

            auto strict_transition_builder = make_shared<path_extend::validation::StrictTransitionStorageBuilder>();
            auto approximate_transition_builder = make_shared<path_extend::validation::ApproximateTransitionStorageBuilder>();

            auto reference_transition_storage =
                BuildFilteredTransitionStorage(contig_path_builder, strict_transition_builder,
                                               unique_storage, path_to_reference);
            name_to_transition_storage.insert({"Reference", reference_transition_storage});

            auto contig_transition_storage =
                BuildFilteredTransitionStorage(contig_path_builder, approximate_transition_builder,
                                               unique_storage, path_to_base_contigs);
            name_to_transition_storage.insert({"Contig", contig_transition_storage});

            auto cloud_transition_storage =
                BuildFilteredTransitionStorage(contig_path_builder, approximate_transition_builder,
                                               unique_storage, path_to_cloud_contigs);
            name_to_transition_storage.insert({"Read cloud contig", contig_transition_storage});

            return name_to_transition_storage;
        }

        void CountBasicClusterStats(const cluster_storage::ClusterStorage& cluster_storage,
                                    const cluster_storage::ClusterGraphAnalyzer& ordering_analyzer) {
            size_t clusters = 0;
            size_t eulerian_clusters = 0;
            size_t path_clusters = 0;

            for (const auto& entry: cluster_storage) {
                auto cluster = entry.second;
                if (cluster.Size() >= 2 and cluster.GetReads() >= configs_.global_read_cluster_threshold) {
                    ++clusters;
                    if (ordering_analyzer.IsEulerianCluster(cluster)) {
                        ++eulerian_clusters;
                        if (ordering_analyzer.IsPathCluster(cluster)) {
                            TRACE("Is path cluster");
                            ++path_clusters;
                        } else {
                            TRACE("Not a path cluster");
                        }
                    }
                }
            }
            INFO(eulerian_clusters << " Eulerian clusters");
            INFO(clusters << " clusters");
            INFO(path_clusters << " path clusters.");
        }

        scaffold_graph_utils::ScaffoldGraphStorage BuildScaffoldGraphStorage(const debruijn_graph::conj_graph_pack& graph_pack,
                                                                             const path_extend::ScaffoldingUniqueEdgeStorage& unique_storage,
                                                                             const path_extend::ScaffolderParams& params,
                                                                             const string& initial_name,
                                                                             const string& score_name,
                                                                             const string& pe_name,
                                                                             const string& connection_name,
                                                                             const string& ordering_name,
                                                                             const string& transitive_name) {
            scaffold_graph_utils::ScaffoldGraphStorage storage;
            size_t scaffolding_distance = params.initial_distance_;
            size_t tail_threshold = params.tail_threshold_;
            size_t count_threshold = params.count_threshold_;
            double score_threshold = params.score_threshold_;

            path_extend::ReadCloudMiddleDijkstraParams long_gap_params(params.connection_score_threshold_,
                                                               params.count_threshold_,
                                                               params.connection_count_threshold_,
                                                               params.tail_threshold_,
                                                               params.connection_length_threshold_,
                                                               params.initial_distance_);

            INFO("Constructing scaffold graph");
            auto scaffold_helper = scaffold_graph_utils::ScaffoldGraphConstructor(unique_storage,
                                                                                  scaffolding_distance,
                                                                                  gp_.g);
            auto scaffold_graph = scaffold_helper.ConstructScaffoldGraphUsingDijkstra();
            INFO(scaffold_graph.VertexCount() << " vertices and " << scaffold_graph.EdgeCount() << " edges in scaffold graph");

            INFO("Constructing score scaffold graph");

            auto score_scaffold_graph = scaffold_helper.ConstructBarcodeScoreScaffoldGraph(scaffold_graph, *barcode_extractor_ptr_,
                                                                                           gp_.g, count_threshold, tail_threshold,
                                                                                           score_threshold);
            INFO(score_scaffold_graph.VertexCount() << " vertices and " << score_scaffold_graph.EdgeCount() << " edges in score scaffold graph");

            INFO("Constructing paired end scaffold graph");

            auto paired_end_scaffold_graph = scaffold_helper.ConstructPairedEndScaffoldGraph(score_scaffold_graph, params,
                                                                                             gp_, unique_storage);

            INFO(score_scaffold_graph.VertexCount() << " vertices and " << score_scaffold_graph.EdgeCount() << " edges in paired end scaffold graph");

//            INFO("Constructing connection scaffold graph");
//
//            auto connection_scaffold_graph = scaffold_helper.ConstructLongGapScaffoldGraph(score_scaffold_graph, unique_storage,
//                                                                                           *barcode_extractor_ptr_, gp_.g,
//                                                                                           long_gap_params);
//            INFO(connection_scaffold_graph.VertexCount() << " vertices and "
//                                                         << connection_scaffold_graph.EdgeCount()
//                                                         << " edges in connection scaffold graph");
//
//            auto ordering_scaffold_graph = scaffold_helper.ConstructOrderingScaffoldGraph(connection_scaffold_graph,
//                                                                                          *barcode_extractor_ptr_, gp_.g,
//                                                                                          long_gap_params.count_threshold_,
//                                                                                          params.split_procedure_strictness_);
//            auto transitive_scaffold_graph = scaffold_helper.ConstructNonTransitiveGraph(ordering_scaffold_graph, gp_.g);


            storage.insert(initial_name, scaffold_graph);
            storage.insert(score_name, score_scaffold_graph);
            storage.insert(pe_name, paired_end_scaffold_graph);
//            storage.insert(connection_name, connection_scaffold_graph);
//            storage.insert(ordering_name, ordering_scaffold_graph);
//            storage.insert(transitive_name, transitive_scaffold_graph);
            return storage;
        }

        void AnalyzeScaffoldGraph(const string& stats_base_path) {
            const string reference_path = cfg::get().ts_res.statistics.genome_path;
            const string cloud_contigs_path = cfg::get().ts_res.statistics.cloud_contigs_path;
            INFO("Reference path: " << reference_path);
            INFO("Cloud contig path: " << cloud_contigs_path);

            const size_t min_length = 5000;
            path_extend::ScaffoldingUniqueEdgeStorage large_unique_storage;
            path_extend::ScaffoldingUniqueEdgeAnalyzer unique_edge_analyzer(gp_, min_length, 100);
            unique_edge_analyzer.FillUniqueEdgeStorage(large_unique_storage);

            ContigPathBuilder contig_path_builder(gp_);
            auto named_reference_paths = contig_path_builder.GetContigPaths(reference_path);
            auto reference_paths = contig_path_builder.StripNames(named_reference_paths);
            INFO(reference_paths.size() << " reference paths");
            ContigPathFilter contig_path_filter(large_unique_storage);
            auto filtered_reference_paths = contig_path_filter.FilterPathsUsingUniqueStorage(reference_paths);

            path_extend::ScaffolderParamsConstructor params_constructor;
            auto params = params_constructor.ConstructScaffolderParams(min_length);
            const string initial_name = "Initial scaffold graph";
            const string score_name = "Score scaffold graph";
            const string pe_name = "Paired end scaffold graph";
            const string connection_name = "Connection scaffold graph";
            const string ordering_name = "Ordering scaffold graph";
            const string transitive_name = "Transitive graph";

//            scaffolder_statistics::ScaffolderStageAnalyzer stats_extractor(gp_.g, large_unique_storage, params,
//                                                                           reference_paths, barcode_extractor_ptr_);
//            stats_extractor.FillStatistics();
//            stats_extractor.SerializeStatistics(stats_base_path);

            //scaffold graph stages table
            auto scaffold_graph_storage = BuildScaffoldGraphStorage(gp_,large_unique_storage, params, initial_name,
                                                                    score_name, pe_name, connection_name, ordering_name,
                                                                    transitive_name);
            scaffold_graph_utils::ScaffolderAnalyzer scaffolder_analyzer(filtered_reference_paths, scaffold_graph_storage, gp_.g);
            scaffolder_analyzer.FillStatistics();
            INFO("Printing stats")
            scaffolder_analyzer.SerializeStatistics(stats_base_path);

            //scaffold graph printing
//            INFO("Getting graph");
//            auto transitive_scaffold_graph = scaffold_graph_storage.at(transitive_name).graph;
//            INFO("Printing graph");
//            scaffold_graph_utils::MetaScaffoldGraphPrinter meta_printer(gp_.g);
//            string scaffold_output_path = fs::append_path(stats_base_path, "scaffold_graph_output");
//            fs::remove_if_exists(scaffold_output_path);
//            fs::make_dir(scaffold_output_path);
//            meta_printer.PrintGraphAsMultiple(transitive_scaffold_graph, named_reference_paths, scaffold_output_path);
        }

        void AnalyzeScaffoldGapCloser(const string& stats_base_path) {
            const size_t large_length_threshold = cfg::get().ts_res.very_long_edge_length;
            const size_t small_length_threshold = cfg::get().ts_res.long_edge_length;
            const string path_to_reference = cfg::get().ts_res.statistics.genome_path;
            path_extend::validation::FilteredReferencePathHelper helper(gp_);
            auto short_reference_paths = helper.GetFilteredReferencePathsFromLength(path_to_reference,
                                                                                       small_length_threshold);

            const size_t count_threshold = 2;
            barcode_index::FrameBarcodeIndexInfoExtractor barcode_extractor(gp_.barcode_mapper_ptr, gp_.g);

            path_extend::ScaffoldingUniqueEdgeAnalyzer analyzer(gp_, small_length_threshold, 100);
            path_extend::ScaffoldingUniqueEdgeStorage unique_storage;
            analyzer.FillUniqueEdgeStorage(unique_storage);
            auto scaffold_helper = scaffold_graph_utils::ScaffoldGraphConstructor(unique_storage,
                                                                                  100000,
                                                                                  gp_.g);
            auto initial_scaffold_graph = scaffold_helper.ConstructScaffoldGraphUsingDijkstra();

            //Check path cluster methods
//            const size_t linkage_distance = 10000;
//            const size_t read_threshold = 15;
//            const size_t num_threads = cfg::get().max_threads;
//            auto barcode_extractor_ptr = make_shared<barcode_index::FrameBarcodeIndexInfoExtractor>(gp_.barcode_mapper_ptr, gp_.g);
//            cluster_storage::ClusterStorageHelper cluster_storage_helper(gp_.g, barcode_extractor_ptr, unique_storage,
//                                                                         linkage_distance, read_threshold, num_threads);
//            auto cluster_storage = cluster_storage_helper.ConstructClusterStorage(initial_scaffold_graph);
//            INFO(cluster_storage.Size() << " clusters");
//            contracted_graph::ContractedGraphFactoryHelper contracted_helper(gp_.g);
//            cluster_storage::ClusterGraphAnalyzer cluster_graph_analyzer(contracted_helper);
//            auto path_cluster_filter_ptr = make_shared<cluster_storage::PathClusterFilter>(cluster_graph_analyzer);
//            cluster_storage::ClusterStorageExtractor cluster_extractor;
//            INFO("Processing");
//            auto path_clusters = cluster_extractor.FilterClusterStorage(cluster_storage, path_cluster_filter_ptr);
//            INFO(path_clusters.size() << " path clusters");
//            auto path_cluster_extractor = make_shared<path_extend::transitions::PathClusterTransitionExtractor>(cluster_graph_analyzer);
//            path_extend::transitions::ClusterTransitionStorageBuilder transition_storage_builder;
//            INFO("Building transition storage");
//            transition_storage_builder.BuildFromClusters(path_clusters, path_cluster_extractor);
//            path_extend::transitions::ClusterTransitionStorage transition_storage = *(transition_storage_builder.GetStorage());
//            INFO("Transition storage size: " << transition_storage.size());

//            scaffolder_statistics::PathClusterScoreAnalyzer path_cluster_analyzer(gp_.g, short_reference_paths,
//                                                                                          transition_storage, path_clusters,
//                                                                                          cluster_graph_analyzer);
//
//            path_cluster_analyzer.FillStatistics();
//            path_cluster_analyzer.SerializeStatistics(stats_base_path);
//
//            const size_t distance_threshold = 3;
//            const double share_threshold = 0.15;

//            Validate subgraph extraction
//            path_extend::CloudScaffoldSubgraphExtractor subgraph_extractor(gp_.g, *barcode_extractor_ptr, distance_threshold,
//                                                                  share_threshold, count_threshold, small_length_threshold,
//                                                                  large_length_threshold);
//            auto large_scaffold_graph = gp_.scaffold_graph_storage.GetLargeScaffoldGraph();
//            auto small_scaffold_graph = gp_.scaffold_graph_storage.GetSmallScaffoldGraph();
//            auto long_reference_paths = helper.GetFilteredReferencePathsFromLength(path_to_reference, large_length_threshold);
//            SubgraphExtractorAnalyzer gap_closer_analyzer(gp_.g, long_reference_paths, short_reference_paths, subgraph_extractor);
//            gap_closer_analyzer.AnalyzeGapCloser(large_scaffold_graph, small_scaffold_graph);
//
//
            //Validate path extraction strategies
//            scaffolder_statistics::GapCloserDijkstraAnalyzer path_extraction_analyzer(gp_.g, short_reference_paths, barcode_extractor,
//                                                                                  count_threshold, small_length_threshold,
//                                                                                  large_length_threshold);
//            path_extraction_analyzer.FillStatistics();
//            path_extraction_analyzer.SerializeStatistics(stats_base_path);

            scaffolder_statistics::GapCloserPathClusterAnalyzer path_cluster_analyzer(gp_, short_reference_paths);
            path_cluster_analyzer.FillStatistics();
            path_cluster_analyzer.SerializeStatistics(stats_base_path);
        }

        void AnalyzePathClusters(const string& stats_base_path, size_t distance) {
            INFO("Distance: " << distance);
            contracted_graph::ContractedGraphFactoryHelper contracted_graph_builder(gp_.g);
            INFO("Building contracted graph");

            ClusterGraphAnalyzerTester tester(gp_, contracted_graph_builder);
            tester.LaunchTests();

            auto contracted_graph = contracted_graph_builder.ConstructFromUniqueStorage(unique_storage_);
            auto scaffold_graph_constructor = scaffold_graph_utils::ScaffoldGraphConstructor(unique_storage_, distance, gp_.g);
            auto scaffold_graph = scaffold_graph_constructor.ConstructScaffoldGraphUsingDijkstra();
            auto contracted_scaffold_graph = scaffold_graph_constructor.ConstructScaffoldGraphFromContractedGraph(contracted_graph);
            INFO("Scaffold graph edges: " << scaffold_graph.EdgeCount());
            INFO("Contracted scaffold graph edges: " << contracted_scaffold_graph.EdgeCount());

            const size_t edge_read_threshold = configs_.edge_read_cluster_threshold;
            const size_t analyzer_read_threshold = configs_.global_read_cluster_threshold;
            const size_t num_threads = cfg::get().max_threads;
            auto cluster_storage_helper = cluster_storage::ClusterStorageHelper(gp_.g, barcode_extractor_ptr_,
                                                                                 unique_storage_, distance,
                                                                                 edge_read_threshold, num_threads);
            auto cluster_storage = cluster_storage_helper.ConstructClusterStorage(scaffold_graph);

            auto min_read_filter = make_shared<cluster_storage::MinReadClusterFilter>(analyzer_read_threshold);
            cluster_storage::ClusterStorageExtractor cluster_storage_extractor;
            auto high_covered_clusters = cluster_storage_extractor.FilterClusterStorage(cluster_storage, min_read_filter);


            cluster_storage::ClusterGraphAnalyzer ordering_analyzer(contracted_graph_builder);

            CountBasicClusterStats(cluster_storage, ordering_analyzer);

            INFO("Building reference transition storage")
            const string path_to_reference = cfg::get().ts_res.statistics.genome_path;
            path_extend::validation::ContigPathBuilder contig_path_builder(gp_);
            auto transition_builder = make_shared<path_extend::validation::GeneralTransitionStorageBuilder>(gp_.g, 1, false, false);
            auto reference_transition_storage = BuildFilteredTransitionStorage(contig_path_builder, transition_builder,
                                                                               unique_storage_, path_to_reference);
            INFO("Reference transition storage size: " << reference_transition_storage.size());

            INFO("Building path cluster storage")
            cluster_statistics::PathClusterStorageBuilder path_cluster_builder;
            auto path_cluster_storage =
                path_cluster_builder.BuildPathClusterStorage(ordering_analyzer, high_covered_clusters);
            INFO(path_cluster_storage.Size() << " distinct clusters");

            INFO("Analyzing path clusters")
            cluster_statistics::ClusterStorageAnalyzer cluster_analyzer(ordering_analyzer, scaffold_graph,
                                                                        reference_transition_storage,
                                                                        path_cluster_storage, high_covered_clusters);

            cluster_analyzer.FillStatistics();
            cluster_analyzer.SerializeStatistics(stats_base_path);

            INFO("Analyzing local contracted graph stats")
            contracted_graph::ContractedGraphClusterStatistics contracted_cluster_analyzer(gp_.g, contracted_graph,
                                                                                           ordering_analyzer,
                                                                                           high_covered_clusters,
                                                                                           reference_transition_storage);
            contracted_cluster_analyzer.FillStatistics();
            contracted_cluster_analyzer.SerializeStatistics(stats_base_path);
        }

        void AnalyzeContigPaths(const string& stats_base_path) {
            const string reference_path = cfg::get().ts_res.statistics.genome_path;
            const string cloud_contigs_path = cfg::get().ts_res.statistics.cloud_contigs_path;
            INFO("Reference path: " << reference_path);
            INFO("Cloud contig path: " << cloud_contigs_path);

            path_extend::validation::ContigPathBuilder contig_path_builder(gp_);
            path_extend::validation::StrictTransitionStorageBuilder strict_transition_builder;
            auto named_reference_paths = contig_path_builder.GetContigPaths(reference_path);
            auto reference_paths = contig_path_builder.StripNames(named_reference_paths);
            auto named_contig_paths = contig_path_builder.GetContigPaths(cloud_contigs_path);
            auto contig_paths = contig_path_builder.StripNames(named_contig_paths);
            INFO(reference_paths.size() << " reference paths");
            INFO(contig_paths.size() << " contig paths");

            const size_t min_length = 20000;
            path_extend::ScaffoldingUniqueEdgeStorage large_unique_storage;
            path_extend::ScaffoldingUniqueEdgeAnalyzer unique_edge_analyzer(gp_, min_length, 100);
            unique_edge_analyzer.FillUniqueEdgeStorage(large_unique_storage);

            INFO("Constructing scaffold graph");
            path_extend::ScaffolderParamsConstructor params_constructor;
            auto scaffolding_params = params_constructor.ConstructScaffolderParams(min_length);
            const size_t scaffolding_distance = scaffolding_params.initial_distance_;
            auto scaffold_helper = scaffold_graph_utils::ScaffoldGraphConstructor(large_unique_storage,
                                                                                  scaffolding_distance,
                                                                                  gp_.g);
            auto scaffold_graph = scaffold_helper.ConstructScaffoldGraphUsingDijkstra();
            INFO(scaffold_graph.VertexCount() << " vertices and " << scaffold_graph.EdgeCount() << " edges in scaffold graph");

            path_extend::validation::ContigPathFilter contig_path_filter(large_unique_storage);
            auto filtered_reference_paths = contig_path_filter.FilterPathsUsingUniqueStorage(reference_paths);
            auto filtered_contig_paths = contig_path_filter.FilterPathsUsingUniqueStorage(contig_paths);
            INFO(filtered_reference_paths.size() << " filtered reference paths");
            INFO(filtered_contig_paths.size() << " filtered contig paths");

            const string reference_output_path = fs::append_path(stats_base_path, "reference_paths");
            ofstream fout_reference(reference_output_path);
            PrintContigPaths(reference_paths, fout_reference);

            const string filtered_output_path = fs::append_path(stats_base_path, "filtered_reference_paths");
            ofstream fout_filt_ref(filtered_output_path);
            PrintContigPaths(filtered_reference_paths, fout_filt_ref);

//            transitions::PathStatisticsExtractor path_stats_extractor(min_length, filtered_reference_paths,
//                                                                      filtered_contig_paths, gp_.g);
//            path_stats_extractor.FillStatistics();
//            path_stats_extractor.SerializeStatistics(stats_base_path);


            INFO("Constructing initial tenx filter");
            auto tenx_resolver_configs = cfg::get().ts_res.tenx;
            typedef barcode_index::FrameBarcodeIndexInfoExtractor barcode_extractor_t;
            auto barcode_extractor_ptr = make_shared<barcode_extractor_t>(gp_.barcode_mapper_ptr, gp_.g);
            path_extend::InitialTenXFilter initial_tenx_filter(gp_.g, barcode_extractor_ptr, tenx_resolver_configs);

            INFO("Constructing initial filter analyzer");
            transitions::InitialFilterStatisticsExtractor initial_filter_extractor(scaffold_graph,
                                                                                   reference_paths,
                                                                                   barcode_extractor_ptr,
                                                                                   initial_tenx_filter,
                                                                                   gp_.g, large_unique_storage);
            INFO("Analyzing initial filter");
            initial_filter_extractor.FillStatistics();
            initial_filter_extractor.SerializeStatistics(stats_base_path);
        }

        void PrintContigPaths(const vector<vector<path_extend::validation::EdgeWithMapping>>& contig_paths, ostream& stream) {
            size_t path_id = 0;
            for (const auto& path: contig_paths) {
                stream << "Path id: " << path_id << std::endl;
                stream << path.size() << " edges." << std::endl;
                for (const auto& edge: path) {
                    stream << edge.edge_.int_id() << std::endl;
                }
                ++path_id;
            }
        }

        void AnalyzeContractedGraph(const string& stats_base_path, size_t distance) {
            INFO("Distance: " << distance);
            contracted_graph::ContractedGraphFactoryHelper contracted_graph_builder(gp_.g);
            INFO("Building contracted graph");
            auto contracted_graph = contracted_graph_builder.ConstructFromUniqueStorage(unique_storage_);
            auto scaffold_graph_constructor = scaffold_graph_utils::ScaffoldGraphConstructor(unique_storage_, distance, gp_.g);
            auto scaffold_graph = scaffold_graph_constructor.ConstructScaffoldGraphUsingDijkstra();
            auto contracted_scaffold_graph = scaffold_graph_constructor.ConstructScaffoldGraphFromContractedGraph(contracted_graph);
            INFO("Scaffold graph edges: " << scaffold_graph.EdgeCount());
            INFO("Contracted scaffold graph edges: " << contracted_scaffold_graph.EdgeCount());
            const size_t edge_read_threshold = configs_.edge_read_cluster_threshold;
            const size_t analyzer_read_threshold = configs_.global_read_cluster_threshold;
            const size_t num_threads = cfg::get().max_threads;
            auto cluster_storage_helper = cluster_storage::ClusterStorageHelper(gp_.g, barcode_extractor_ptr_,
                                                                                unique_storage_, distance,
                                                                                edge_read_threshold, num_threads);
            auto cluster_storage = cluster_storage_helper.ConstructClusterStorage(scaffold_graph);


            cluster_storage::ClusterGraphAnalyzer ordering_analyzer(contracted_graph_builder);
            auto min_read_filter = make_shared<cluster_storage::MinReadClusterFilter>(analyzer_read_threshold);
            cluster_storage::ClusterStorageExtractor cluster_extractor;
            auto high_covered_clusters = cluster_extractor.FilterClusterStorage(cluster_storage, min_read_filter);

            cluster_statistics::PathClusterStorageBuilder path_cluster_builder;
            auto path_cluster_storage =
                path_cluster_builder.BuildPathClusterStorage(ordering_analyzer, high_covered_clusters);
            INFO(path_cluster_storage.Size() << " distinct clusters");

            auto name_to_transition_storage = BuildTransitionStorages(unique_storage_);
            auto reference_transition_storage = name_to_transition_storage.at("Reference");

            scaffold_graph_utils::ScaffoldGraphAnalyzer scaffold_analyzer(contracted_scaffold_graph);
            scaffold_analyzer.FillStatistics();
            scaffold_analyzer.SerializeStatistics(stats_base_path);

            contracted_graph::ContractedGraphAnalyzer contracted_analyzer(gp_.g, ordering_analyzer,
                                                                          *barcode_extractor_ptr_, path_cluster_storage,
                                                                          contracted_graph, name_to_transition_storage,
                                                                          reference_transition_storage,
                                                                          cluster_storage,
                                                                          analyzer_read_threshold);
            contracted_analyzer.FillStatistics();
            contracted_analyzer.SerializeStatistics(stats_base_path);
        }

        void LaunchAnalyzerForMultipleDistances(const string& stats_base_path,
                                                const std::function<void(const string&, size_t)> analyzer_function,
                                                const vector<size_t>& distances, size_t threads) {
#pragma omp parallel for num_threads(threads)
            for (size_t i = 0; i < distances.size(); ++i) {
                size_t distance = distances[i];
                string stat_path = fs::append_path(stats_base_path, "distance_" + std::to_string(distance));
                fs::make_dir(stat_path);
                INFO(stat_path);
                analyzer_function(stat_path, distance);
            }
        }
        DECL_LOGGER("StatisticsLauncher");
    };
}