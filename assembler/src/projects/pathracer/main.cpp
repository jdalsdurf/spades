//***************************************************************************
//* Copyright (c) 2018 Saint Petersburg State University
//* All Rights Reserved
//* See file LICENSE for details.
//***************************************************************************

#include "fees.hpp"
#include "omnigraph_wrapper.hpp"
#include "depth_filter.hpp"
#include "cursor_conn_comps.hpp"

#include "assembly_graph/core/graph.hpp"
#include "assembly_graph/dijkstra/dijkstra_helper.hpp"
#include "assembly_graph/components/graph_component.hpp"
#include "assembly_graph/paths/bidirectional_path_io/bidirectional_path_output.hpp"

#include "sequence/aa.hpp"

#include "visualization/visualization.hpp"
#include "pipeline/graphio.hpp"
#include "io/graph/gfa_reader.hpp"
#include "io/reads/io_helper.hpp"
#include "io/reads/osequencestream.hpp"
#include "io/reads/file_reader.hpp"

#include "hmm/hmmfile.hpp"
#include "hmm/hmmmatcher.hpp"

#include "utils/parallel/openmp_wrapper.h"
#include "utils/logger/log_writers.hpp"
#include "utils/segfault_handler.hpp"

#include "version.hpp"

#include <clipp/clipp.h>
#include <debug_assert/debug_assert.hpp>

#include <sys/types.h>
#include <sys/stat.h>
#include <string>

#include <llvm/ADT/iterator_range.h>
#include <type_traits>

extern "C" {
    #include "easel.h"
    #include "esl_sqio.h"
    #include "esl_vectorops.h"
}

struct main_assert : debug_assert::default_handler,
                     debug_assert::set_level<1> {};

void create_console_logger(const std::string &filename = "") {
    using namespace logging;

    logger *lg = create_logger("");
    lg->add_writer(std::make_shared<mutex_writer>(std::make_shared<console_writer>()));
    if (filename != "") {
        lg->add_writer(std::make_shared<mutex_writer>(std::make_shared<file_writer>(filename)));
    }
    attach_logger(lg);
}

template <typename Range, typename Sep>
std::string join(const Range &range, const Sep &sep) {
    std::stringstream ss;
    size_t inserted = 0;
    for (const auto &e : range) {
        if (inserted > 0) {
            ss << sep;
        }
        ss << e;
        ++inserted;
    }

    return ss.str();
}

enum class mode {
    hmm,
    nucl,
    aa
};

struct cfg {
    std::string load_from = "";
    std::string hmmfile = "";
    std::string output_dir = "";
    enum mode mode = mode::hmm;
    size_t k = 0;
    int threads = 4;
    size_t top = 100;
    uint64_t int_id = 0;
    unsigned max_size = 10000000;
    bool debug = false;
    bool draw = false;
    bool save = true;
    bool rescore = true;
    bool annotate_graph = true;

    hmmer::hmmer_cfg hcfg;
};

extern "C" {
#include "p7_config.h"

#include <stdlib.h>
#include <string.h>

#include "easel.h"
#include "esl_alphabet.h"
#include "esl_sq.h"

#include "hmmer.h"
}

void process_cmdline(int argc, char **argv, cfg &cfg) {
  using namespace clipp;

  auto cli = (
      cfg.hmmfile    << value("input file (HMM or sequence)"),
      cfg.load_from  << value("load from"),
      cfg.k          << integer("k-mer size"),
      required("--output", "-o") & value("output directory", cfg.output_dir)    % "output directory",
      one_of(option("--hmm").set(cfg.mode, mode::hmm) % "match against HMM(s) [default]",
             option("--nucl").set(cfg.mode, mode::nucl) % "match against nucleotide string(s)",
             option("--aa").set(cfg.mode, mode::aa) % "match agains amino acid string(s)"),
      (option("--top") & integer("x", cfg.top)) % "extract top x paths [default: 100]",
      (option("--threads", "-t") & integer("value", cfg.threads)) % "number of threads",
      (option("--edge-id") & integer("value", cfg.int_id)) % "match around edge",
      (option("--max_size") & integer("value", cfg.max_size)) % "maximal component size to consider [default: 10000000]",
      // Control of output
      cfg.hcfg.acc     << option("--acc")          % "prefer accessions over names in output",
      cfg.hcfg.noali   << option("--noali")        % "don't output alignments, so output is smaller",
      // Control of reporting thresholds
      (option("-E") & number("value", cfg.hcfg.E))        % "report sequences <= this E-value threshold in output",
      (option("-T") & number("value", cfg.hcfg.T))        % "report sequences >= this score threshold in output",
      (option("--domE") & number("value", cfg.hcfg.domE)) % "report domains <= this E-value threshold in output",
      (option("--domT") & number("value", cfg.hcfg.domT)) % "report domains >= this score cutoff in output",
      // Control of inclusion (significance) thresholds
      (option("-incE") & number("value", cfg.hcfg.incE))       % "consider sequences <= this E-value threshold as significant",
      (option("-incT") & number("value", cfg.hcfg.incT))       % "consider sequences >= this score threshold as significant",
      (option("-incdomE") & number("value", cfg.hcfg.incdomE)) % "consider domains <= this E-value threshold as significant",
      (option("-incdomT") & number("value", cfg.hcfg.incdomT)) % "consider domains >= this score threshold as significant",
      // Model-specific thresholding for both reporting and inclusion
      cfg.hcfg.cut_ga  << option("--cut_ga")       % "use profile's GA gathering cutoffs to set all thresholding",
      cfg.hcfg.cut_nc  << option("--cut_nc")       % "use profile's NC noise cutoffs to set all thresholding",
      cfg.hcfg.cut_tc  << option("--cut_tc")       % "use profile's TC trusted cutoffs to set all thresholding",
      // Control of acceleration pipeline
      cfg.hcfg.max     << option("--max")             % "Turn all heuristic filters off (less speed, more power)",
      (option("--F1") & number("value", cfg.hcfg.F1)) % "Stage 1 (MSV) threshold: promote hits w/ P <= F1",
      (option("--F2") & number("value", cfg.hcfg.F2)) % "Stage 2 (Vit) threshold: promote hits w/ P <= F2",
      (option("--F3") & number("value", cfg.hcfg.F3)) % "Stage 3 (Fwd) threshold: promote hits w/ P <= F3",
      cfg.debug << option("--debug") % "enable extensive debug output",
      cfg.draw  << option("--draw")  % "draw pictures around the interesting edges",
      cfg.save << option("--save") % "save found sequences",
      cfg.rescore  << option("--rescore")  % "rescore paths via HMMer",
      cfg.annotate_graph << option("--annotate-graph") % "emit paths in GFA graph"
  );

  if (!parse(argc, argv, cli)) {
    std::cout << make_man_page(cli, argv[0]);
    exit(1);
  }
}

void DrawComponent(const omnigraph::GraphComponent<debruijn_graph::ConjugateDeBruijnGraph> &component,
                   const debruijn_graph::ConjugateDeBruijnGraph &graph,
                   const std::string &prefix,
                   const std::vector<debruijn_graph::EdgeId> &match_edges) {
    using namespace visualization;
    using namespace visualization::visualization_utils;
    using namespace debruijn_graph;

    // FIXME: This madness needs to be refactored
    graph_labeler::StrGraphLabeler<ConjugateDeBruijnGraph> tmp_labeler1(graph);
    graph_labeler::CoverageGraphLabeler<ConjugateDeBruijnGraph> tmp_labeler2(graph);
    graph_labeler::CompositeLabeler<ConjugateDeBruijnGraph> labeler{tmp_labeler1, tmp_labeler2};

    auto colorer = graph_colorer::DefaultColorer(graph);
    auto edge_colorer = std::make_shared<graph_colorer::CompositeEdgeColorer<ConjugateDeBruijnGraph>>("black");
    edge_colorer->AddColorer(colorer);
    edge_colorer->AddColorer(std::make_shared<graph_colorer::SetColorer<ConjugateDeBruijnGraph>>(graph, match_edges, "green"));
    std::shared_ptr<graph_colorer::GraphColorer<ConjugateDeBruijnGraph>>
            resulting_colorer = std::make_shared<graph_colorer::CompositeGraphColorer<Graph>>(colorer, edge_colorer);

    WriteComponent(component,
                   prefix + ".dot",
                   resulting_colorer,
                   labeler);
}


template <typename... Ts> using void_t = void;

template <typename T, typename = void>
struct has_edge_method : std::false_type {};

template <typename T>
struct has_edge_method<T, void_t<decltype(T{}.edge())>> : std::true_type {};

template<class GraphCursor>
std::enable_if_t<has_edge_method<GraphCursor>::value, std::vector<typename GraphCursor::EdgeId>> to_path(const std::vector<GraphCursor> &cpath) {
    std::vector<typename GraphCursor::EdgeId> path;

    size_t prev_position = 0;
    for (auto cursor : cpath) {
        const auto e = cursor.edge();
        size_t position = cursor.position();
        if (path.empty() || e != path.back() || prev_position >= position) {
            path.push_back(e);
        }
        prev_position = position;
    }

    return path;
}

template<class GraphCursor>
std::vector<typename GraphCursor::EdgeId> to_path(const std::vector<AAGraphCursor<GraphCursor>> &cpath) {
    return to_path(to_nucl_path(cpath));
}

using debruijn_graph::EdgeId;
using debruijn_graph::VertexId;
using debruijn_graph::ConjugateDeBruijnGraph;
using EdgeAlnInfo = std::vector<std::pair<EdgeId, std::pair<int, int>>>;


auto ScoreSequences(const std::vector<std::string> &seqs,
                    const std::vector<std::string> &refs,
                    const hmmer::HMM &hmm, const cfg &cfg) {
    bool hmm_in_aas = hmm.abc()->K == 20;
    hmmer::HMMMatcher matcher(hmm, cfg.hcfg);

    if (!hmm_in_aas) {
        INFO("HMM in nucleotides");
    } else {
        INFO("HMM in amino acids");
    }

    for (size_t i = 0; i < seqs.size(); ++i) {
        std::string ref = refs.size() > i ? refs[i] : std::to_string(i);
        if (!hmm_in_aas) {
            matcher.match(ref.c_str(), seqs[i].c_str());
        } else {
            for (size_t shift = 0; shift < 3; ++shift) {
                std::string ref_shift = ref + "/" + std::to_string(shift);
                std::string seq_aas = aa::translate(seqs[i].c_str() + shift);
                matcher.match(ref_shift.c_str(), seq_aas.c_str());
            }
        }
    }

    matcher.summarize();
    return matcher;
}

EdgeAlnInfo get_matched_edges(const std::vector<EdgeId> &edges,
                              const hmmer::HMMMatcher &matcher,
                              const cfg &cfg) {
    EdgeAlnInfo match_edges;
    for (const auto &hit : matcher.hits()) {
        if (!hit.reported() || !hit.included())
            continue;

        EdgeId e = edges[std::stoull(hit.name())];
        if (cfg.debug)
            INFO("HMMER seq id:" <<  hit.name() << ", edge id:" << e);

        for (const auto &domain : hit.domains()) {
            // Calculate HMM overhang
            std::pair<int, int> seqpos = domain.seqpos();
            std::pair<int, int> hmmpos = domain.hmmpos();

            int roverhang = static_cast<int>(domain.M() - hmmpos.second) - static_cast<int>(domain.L() - seqpos.second);
            int loverhang = static_cast<int>(hmmpos.first) - static_cast<int>(seqpos.first);

            match_edges.push_back({e, std::make_pair(loverhang, roverhang)});
        }
    }
    INFO("Total matched edges: " << match_edges.size());

    return match_edges;
}

EdgeAlnInfo MatchedEdges(const std::vector<EdgeId> &edges,
                         const ConjugateDeBruijnGraph &graph,
                         const hmmer::HMM &hmm, const cfg &cfg) {
    std::vector<std::string> seqs;
    for (size_t i = 0; i < edges.size(); ++i) {
        seqs.push_back(graph.EdgeNucls(edges[i]).str());
    }
    auto matcher = ScoreSequences(seqs, {}, hmm, cfg);

    auto match_edges = get_matched_edges(edges, matcher, cfg);

    if (match_edges.size() && cfg.debug) {
        int textw = 120;
        #pragma omp critical(console)
        {
            p7_tophits_Targets(stdout, matcher.top_hits(), matcher.pipeline(), textw);
            p7_tophits_Domains(stdout, matcher.top_hits(), matcher.pipeline(), textw);
            p7_pli_Statistics(stdout, matcher.pipeline(), nullptr);
        }
    }

    return match_edges;
}

void OutputMatches(const hmmer::HMM &hmm, const hmmer::HMMMatcher &matcher, const std::string &filename,
                   const std::string &format = "tblout") {
    P7_HMM *p7hmm = hmm.get();
    FILE *fp = fopen(filename.c_str(), "w");
    if (format == "domtblout") {
        p7_tophits_TabularDomains(fp, p7hmm->name, p7hmm->acc, matcher.top_hits(), matcher.pipeline(), true);
        // TODO Output tail
    } else if (format == "tblout") {
        p7_tophits_TabularTargets(fp, p7hmm->name, p7hmm->acc, matcher.top_hits(), matcher.pipeline(), true);
        // TODO Output tail
    } else if (format == "pfamtblout") {
        p7_tophits_TabularXfam(fp, p7hmm->name, p7hmm->acc, matcher.top_hits(), matcher.pipeline());
    } else {
        FATAL_ERROR("unknown output format");
    }
    fclose(fp);
}

std::string PathToString(const std::vector<EdgeId>& path,
                         const ConjugateDeBruijnGraph &graph) {
    std::string res = "";
    for (const auto &e : path)
        res = res + graph.EdgeNucls(e).First(graph.length(e)).str();
    return res;
}

template<class Graph>
Sequence MergeSequences(const Graph &g,
                        const std::vector<typename Graph::EdgeId> &continuous_path) {
    std::vector<Sequence> path_sequences;
    path_sequences.push_back(g.EdgeNucls(continuous_path[0]));
    for (size_t i = 1; i < continuous_path.size(); ++i) {
        VERIFY(g.EdgeEnd(continuous_path[i - 1]) == g.EdgeStart(continuous_path[i]));
        path_sequences.push_back(g.EdgeNucls(continuous_path[i]));
    }
    return MergeOverlappingSequences(path_sequences, g.k());
}

static bool ends_with(const std::string &s, const std::string &p) {
    if (s.size() < p.size())
        return false;

    return (s.compare(s.size() - p.size(), p.size(), p) == 0);
}

std::vector<hmmer::HMM> ParseHMMFile(const std::string &filename) {
    /* Open the query profile HMM file */
    hmmer::HMMFile hmmfile(filename);
    if (!hmmfile.valid()) {
        FATAL_ERROR("Error opening HMM file " << filename);
    }

    std::vector<hmmer::HMM> hmms;

    while (auto hmmw = hmmfile.read())
        hmms.emplace_back(std::move(hmmw.get()));

    if (hmms.empty()) {
        FATAL_ERROR("Error reading HMM file " << filename);
    }

    return hmms;
}


std::vector<hmmer::HMM> ParseFASTAFile(const std::string &filename, enum mode mode) {
    std::vector<hmmer::HMM> res;
    hmmer::HMMSequenceBuilder builder(mode == mode::nucl ? hmmer::Alphabet::DNA : hmmer::Alphabet::AMINO,
                                      hmmer::ScoreSystem::Default);

    ESL_ALPHABET   *abc  = esl_alphabet_Create(mode == mode::nucl ? eslDNA : eslAMINO);
    ESL_SQ         *qsq  = esl_sq_CreateDigital(abc);
    ESL_SQFILE     *qfp  = NULL;
    const char *qfile = filename.c_str();

    // Open the query sequence file in FASTA format
    int status = esl_sqfile_Open(qfile, eslSQFILE_FASTA, NULL, &qfp);
    if      (status == eslENOTFOUND) {
        FATAL_ERROR("No such file " << filename);
    } else if (status == eslEFORMAT) {
        FATAL_ERROR("Format of " << filename << " unrecognized.");
    } else if (status == eslEINVAL) {
        FATAL_ERROR("Can't autodetect stdin or .gz.");
    } else if (status != eslOK) {
        FATAL_ERROR("Open of " << filename << " failed, code " << status);
    }

    // For each sequence, build a model and save it.
    while ((status = esl_sqio_Read(qfp, qsq)) == eslOK) {
        INFO("Converting " << qsq->name << ", len: " << qsq->n);
        res.push_back(builder.from_string(qsq));
        esl_sq_Reuse(qsq);
    }
    if (status != eslEOF) {
        FATAL_ERROR("Unexpected error " << status << " reading sequence file " << filename);
    }

    esl_sq_Destroy(qsq);
    esl_sqfile_Close(qfp);
    esl_alphabet_Destroy(abc);

    return res;
}

template <typename Container>
auto EdgesToSequences(const Container &entries,
                      const debruijn_graph::ConjugateDeBruijnGraph &graph) {
    std::vector<std::pair<std::string, std::string>> ids_n_seqs;
    for (const auto &entry : entries) {
        std::string id = join(entry, "_");
        std::string seq = MergeSequences(graph, entry).str();
        ids_n_seqs.push_back({id, seq});
    }

    std::sort(ids_n_seqs.begin(), ids_n_seqs.end());
    return ids_n_seqs;
}

template <typename Container>
void ExportEdges(const Container &entries,
                 const debruijn_graph::ConjugateDeBruijnGraph &graph,
                 const std::string &filename) {
    if (entries.size() == 0)
        return;

    std::ofstream o(filename, std::ios::out);

    for (const auto &kv : EdgesToSequences(entries, graph)) {
        o << ">" << kv.first << "\n";
        io::WriteWrapped(kv.second, o);
    }
}

void LoadGraph(debruijn_graph::ConjugateDeBruijnGraph &graph,
               std::vector<std::vector<EdgeId>> &paths,
               const std::string &filename) {
    using namespace debruijn_graph;
    if (ends_with(filename, ".gfa")) {
        gfa::GFAReader gfa(filename);
        INFO("GFA segments: " << gfa.num_edges() << ", links: " << gfa.num_links());
        gfa.to_graph(graph);
        paths.reserve(gfa.num_paths());
        for (const auto &path : gfa.paths())
            paths.push_back(path.edges);
    } else {
        graphio::ScanBasicGraph(filename, graph);
    }
}

struct HMMPathInfo {
    std::string hmmname;
    unsigned priority;
    double score;
    std::string seq;
    std::string nuc_seq;
    std::vector<EdgeId> path;
    std::string alignment;

    bool operator<(const HMMPathInfo &that) const {
        return score < that.score;
    }

    HMMPathInfo(std::string name, unsigned prio, double sc, std::string s, std::string nuc_s, std::vector<EdgeId> p, std::string alignment)
            : hmmname(std::move(name)), priority(prio), score(sc),
              seq(std::move(s)), nuc_seq{std::move(nuc_s)},
              path(std::move(p)), alignment(std::move(alignment)) {}
};

void SaveResults(const hmmer::HMM &hmm, const ConjugateDeBruijnGraph &graph,
                 const cfg &cfg,
                 const std::vector<HMMPathInfo> &results) {
    const P7_HMM *p7hmm = hmm.get();  // TODO We use only hmm name from this object, may be we should just pass the name itself

    INFO("Total " << results.size() << " resultant paths extracted");

    if (cfg.save && !results.empty()) {
        {
            std::ofstream o(cfg.output_dir + std::string("/") + p7hmm->name + ".seqs.fa", std::ios::out);
            for (const auto &result : results) {
                if (result.seq.size() == 0)
                    continue;
                o << ">Score=" << result.score << "|Edges=" << join(result.path, "_") << "|Alignment=" << result.alignment << '\n';
                io::WriteWrapped(result.seq, o);
            }
        }

        // TODO merge these blocks
        {
            std::ofstream o(cfg.output_dir + std::string("/") + p7hmm->name + ".nucs.fa", std::ios::out);
            for (const auto &result : results) {
                if (result.seq.size() == 0)
                    continue;
                o << ">Score=" << result.score << "|Edges=" << join(result.path, "_") << "|Alignment=" << result.alignment << '\n';
                io::WriteWrapped(result.nuc_seq, o);
            }
        }
        // {
        //     std::ofstream o(cfg.output_dir + std::string("/") + p7hmm->name + ".fa", std::ios::out);
        //     for (const auto &result : results) {
        //         o << ">" << result.leader << "_" << result.priority;
        //         if (result.seq.size() == 0)
        //             o << " (whole edge)";
        //         o << '\n';
        //         if (result.seq.size() == 0) {
        //             io::WriteWrapped(graph.EdgeNucls(result.leader).str(), o);
        //         } else {
        //             io::WriteWrapped(result.seq, o);
        //         }
        //     }
        // }
    }
}

void Rescore(const hmmer::HMM &hmm, const ConjugateDeBruijnGraph &graph,
             const cfg &cfg,
             const std::vector<HMMPathInfo> &results) {
    P7_HMM *p7hmm = hmm.get();

    std::unordered_set<std::vector<EdgeId>> to_rescore;

    for (const auto &result : results) {
        if (result.path.size() == 0)
            continue;

        to_rescore.insert(result.path);
    }

    INFO("Total " << to_rescore.size() << " local paths to rescore");

    ExportEdges(to_rescore, graph, cfg.output_dir + std::string("/") + p7hmm->name + ".edges.fa");

    std::vector<std::string> seqs_to_rescore;
    std::vector<std::string> refs_to_rescore;
    for (const auto &kv : EdgesToSequences(to_rescore, graph)) {
        refs_to_rescore.push_back(kv.first);
        seqs_to_rescore.push_back(kv.second);
    }

    auto matcher = ScoreSequences(seqs_to_rescore, refs_to_rescore, hmm, cfg);
    OutputMatches(hmm, matcher, cfg.output_dir + "/" + p7hmm->name + ".tblout", "tblout");
    OutputMatches(hmm, matcher, cfg.output_dir + "/" + p7hmm->name + ".domtblout", "domtblout");
    OutputMatches(hmm, matcher, cfg.output_dir + "/" + p7hmm->name + ".pfamtblout", "pfamtblout");
}

void TraceHMM(const hmmer::HMM &hmm,
              const debruijn_graph::ConjugateDeBruijnGraph &graph, const std::vector<EdgeId> &edges,
              const cfg &cfg,
              std::vector<HMMPathInfo> &results) {
    const P7_HMM *p7hmm = hmm.get();

    INFO("Query:         " << p7hmm->name << "  [M=" << p7hmm->M << "]");
    if (p7hmm->acc) {
        INFO("Accession:     " << p7hmm->acc);
    }
    if (p7hmm->desc) {
        INFO("Description:   " << p7hmm->desc);
    }

    auto fees = hmm::fees_from_hmm(p7hmm, hmm.abc());
    INFO("HMM consensus: " << fees.consensus);

    // Collect the neighbourhood of the matched edges
    EdgeAlnInfo matched_edges = MatchedEdges(edges, graph, hmm, cfg);  // hhmer is thread-safe
    bool hmm_in_aas = hmm.abc()->K == 20;

    using GraphCursor = DebruijnGraphCursor;
    std::vector<std::pair<GraphCursor, size_t>> left_queries, right_queries;
    std::unordered_set<GraphCursor> cursors;
    for (const auto &kv : matched_edges) {
        EdgeId e = kv.first;
        int aa_coef = hmm_in_aas ? 3 : 1;
        int loverhang = (kv.second.first + 10) * aa_coef; // TODO unify overhangs processing
        int roverhang = (kv.second.second + 10) * aa_coef;

        if (loverhang > 0) {
            for (const auto &start : GraphCursor::get_cursors(graph, e, 0)) {
                left_queries.push_back({start, loverhang * 2});
            }
        }

        size_t len = graph.length(e) + graph.k();
        INFO("Edge length: " << len <<"; edge overhangs: " << loverhang << " " << roverhang);
        if (roverhang > 0) {
            for (const auto &end : GraphCursor::get_cursors(graph, e, len - 1)) {
                right_queries.push_back({end, roverhang * 2});
            }
        }

        for (size_t i = std::max(0, -loverhang); i < len - std::max(0, -roverhang); ++i) {
            auto position_cursors = GraphCursor::get_cursors(graph, e, i);
            cursors.insert(std::make_move_iterator(position_cursors.begin()), std::make_move_iterator(position_cursors.end()));
        }
    }

    auto left_cursors = impl::depth_subset(left_queries, false);
    auto right_cursors = impl::depth_subset(right_queries, true);
    cursors.insert(left_cursors.cbegin(), left_cursors.cend());
    cursors.insert(right_cursors.cbegin(), right_cursors.cend());

    std::vector<GraphCursor> cursors_vector(cursors.cbegin(), cursors.cend());
    auto cursor_conn_comps = cursor_connected_components(cursors_vector);
    std::stable_sort(cursor_conn_comps.begin(), cursor_conn_comps.end(),
                     [](const auto &c1, const auto &c2) { return c1.size() > c2.size(); });

    INFO("The number of connected components: " << cursor_conn_comps.size());
    std::vector<size_t> cursor_conn_comps_sizes;
    for (const auto &comp : cursor_conn_comps) {
        cursor_conn_comps_sizes.push_back(comp.size());
    }
    INFO("Connected component sizes: " << cursor_conn_comps_sizes);

    auto run_search = [&](const auto &initial, size_t top,
                          std::vector<HMMPathInfo> &local_results) {
        auto result = find_best_path(fees, initial);

        INFO("Extracting top paths");
        auto top_paths = result.top_k(top);
        if (!top_paths.empty()) {
            INFO("Best score in the current component: " << result.best_score());
            INFO("Best sequence in the current component");
            INFO(top_paths.str(0));
            INFO("Alignment: " << top_paths.compress_alignment(top_paths.alignment(0, fees)));
        }
        size_t idx = 0;
        for (const auto& annotated_path : top_paths) {
            auto seq = top_paths.str(annotated_path.path);
            auto alignment = top_paths.compress_alignment(top_paths.alignment(annotated_path, fees));
            auto nucl_path = to_nucl_path(annotated_path.path);
            std::string nucl_seq = top_paths.str(nucl_path);
            DEBUG_ASSERT(check_path_continuity(nucl_path), main_assert{}, debug_assert::level<2>{});
            auto edge_path = to_path(nucl_path);
            DEBUG_ASSERT(!edge_path.empty(), main_assert{});
            auto edge_path_aas = to_path(annotated_path.path);
            if (edge_path != edge_path_aas) {
                ERROR("NT: " << edge_path);
                ERROR("AA: " << edge_path_aas);
            }
            DEBUG_ASSERT(edge_path == edge_path_aas, main_assert{}, debug_assert::level<2>{});
            local_results.emplace_back(p7hmm->name, idx++, annotated_path.score, seq, nucl_seq, std::move(edge_path), std::move(alignment));
        }
    };

    std::vector<EdgeId> match_edges;
    for (const auto &entry : matched_edges) {
        match_edges.push_back(entry.first);
    }
    remove_duplicates(match_edges);

    for (const auto &component_cursors : cursor_conn_comps) {
        assert(!component_cursors.empty());
        INFO("Component size " << component_cursors.size());
        if (component_cursors.size() > cfg.max_size) {
            WARN("The component is too large, skipping");
            continue;
        }

        for (const auto &cursor : component_cursors) {
            DEBUG_ASSERT(check_cursor_symmetry(cursor), main_assert{}, debug_assert::level<2>{});
        }

        auto component_name = int_to_hex(hash_value(component_cursors));
        INFO("Construct component " << component_name);
        std::unordered_set<EdgeId> edges;
        for (const auto& cursor : component_cursors) {
            edges.insert(cursor.edge());
        }

        // EdgeId leader = std::max_element(edges_count.cbegin(), edges_count.cend(),
        //                                  [](const auto &kv1, const auto &kv2) { return kv1.second < kv2.second; })->first;
        // INFO("Leader (mostly supported among matched) edge: " << leader << " supported by " << edges_count[leader] << " cursors");

        auto component = omnigraph::GraphComponent<ConjugateDeBruijnGraph>::FromEdges(graph, edges, true, component_name);

        if (cfg.draw) {
            INFO("Writing component " << component_name);
            DrawComponent(component, graph, cfg.output_dir + "/" + component_name, match_edges);
        }

        INFO("Running path search");
        std::vector<HMMPathInfo> local_results;
        std::unordered_set<GraphCursor> component_set(component_cursors.cbegin(), component_cursors.cend());
        auto restricted_component_cursors = make_restricted_cursors(component_cursors, component_set);
        if (hmm_in_aas) {
            run_search(make_aa_cursors(restricted_component_cursors), cfg.top, local_results);
        } else {
            run_search(restricted_component_cursors, cfg.top, local_results);
        }

        results.insert(results.end(), local_results.begin(), local_results.end());

        std::unordered_set<std::vector<EdgeId>> paths;
        for (const auto& entry : local_results)
            paths.insert(entry.path);
        INFO("Total " << paths.size() << " unique edge paths extracted");

        {
            size_t idx = 0;
            for (const auto &path : paths) {
                std::vector<size_t> int_ids;
                for (EdgeId e : path) {
                    int_ids.push_back(e.int_id());
                }
                INFO("Path length : " << path.size() << " edges " << int_ids);

                if (cfg.draw) {
                    INFO("Writing component around path");
                    DrawComponent(component, graph, cfg.output_dir + "/" + component_name + "_" + std::to_string(idx), path);
                }
                idx += 1;
            }
        }
    }
}

void hmm_main(const cfg &cfg,
              const debruijn_graph::ConjugateDeBruijnGraph &graph,
              const std::vector<EdgeId> &edges,
              std::unordered_set<std::vector<EdgeId>> &to_rescore,
              std::set<std::pair<std::string, std::vector<EdgeId>>> &gfa_paths) {
    std::vector<hmmer::HMM> hmms;
    if (cfg.mode == mode::hmm)
        hmms = ParseHMMFile(cfg.hmmfile);
    else
        hmms = ParseFASTAFile(cfg.hmmfile, cfg.mode);

    // Outer loop: over each query HMM in <hmmfile>.
    omp_set_num_threads(cfg.threads);
    #pragma omp parallel for
    for (size_t _i = 0; _i < hmms.size(); ++_i) {
        const auto &hmm = hmms[_i];

        std::vector<HMMPathInfo> results;

        TraceHMM(hmm, graph, edges, cfg, results);

        std::sort(results.begin(), results.end());
        SaveResults(hmm, graph, cfg, results);

        if (cfg.annotate_graph) {
            std::unordered_set<std::vector<EdgeId>> unique_paths;
            for (const auto &result : results)
                unique_paths.insert(result.path);

            size_t idx = 0;
            for (const auto &path : unique_paths) {
                #pragma omp critical
                {
                    gfa_paths.insert({ std::string(hmm.get()->name) + "_" + std::to_string(idx++) + "_length_" + std::to_string(path.size()), path });
                }
            }
        }

        if (cfg.rescore) {
            Rescore(hmm, graph, cfg, results);

            for (const auto &result : results) {
                #pragma omp critical
                {
                    to_rescore.insert(result.path);
                }
            }
        }
    } // end outer loop over query HMMs
}


int main(int argc, char* argv[]) {
    utils::segfault_handler sh;
    utils::perf_counter pc;

    srand(42);
    srandom(42);

    cfg cfg;
    process_cmdline(argc, argv, cfg);

    create_console_logger(cfg.output_dir + "/pathracer.log");

    int status = mkdir(cfg.output_dir.c_str(), 0775);
    if (status != 0) {
        if (errno == EEXIST) {
            WARN("Output directory exists: " << cfg.output_dir);
        } else {
            ERROR("Cannot create output directory: " << cfg.output_dir);
            std::exit(1);
        }
    }

    START_BANNER("Graph HMM aligning engine");
    std::string cmd_line = join(llvm::make_range(argv, argv + argc), " ");
    INFO("Command line: " << cmd_line);

    using namespace debruijn_graph;

    debruijn_graph::ConjugateDeBruijnGraph graph(cfg.k);
    std::vector<std::vector<EdgeId>> contig_paths;
    LoadGraph(graph, contig_paths, cfg.load_from);
    INFO("Graph loaded. Total vertices: " << graph.size());

    // Collect all the edges
    std::vector<EdgeId> edges;
    for (auto it = graph.ConstEdgeBegin(); !it.IsEnd(); ++it) {
        EdgeId edge = *it;
        if (cfg.int_id == 0 || edge.int_id() == cfg.int_id) edges.push_back(edge);
    }

    std::unordered_set<std::vector<EdgeId>> to_rescore;
    std::set<std::pair<std::string, std::vector<EdgeId>>> gfa_paths;

    hmm_main(cfg, graph, edges, to_rescore, gfa_paths);

    if (cfg.rescore) {
        INFO("Total " << to_rescore.size() << " paths to rescore");
        ExportEdges(to_rescore, graph, cfg.output_dir + "/all.edges.fa");
    }

    if (cfg.annotate_graph) {
        std::string fname = cfg.output_dir + "/graph_with_hmm_paths.gfa";
        INFO("Saving annotated graph to " << fname)
        std::ofstream os(fname);
        path_extend::GFAPathWriter gfa_writer(graph, os);
        gfa_writer.WriteSegmentsAndLinks();

        for (const auto& entry : gfa_paths) {
            gfa_writer.WritePaths(entry.second, entry.first);
        }
    }

    return 0;
}

// vim: set ts=4 sw=4 et :