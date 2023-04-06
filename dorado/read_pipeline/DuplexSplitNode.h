#pragma once
#include "ReadPipeline.h"

/*
. put in a PR for end_reason (modify dataloader)
. assert empty mappings
*/
namespace dorado {

struct DuplexSplitSettings {
    bool enabled = true;
    bool simplex_mode;
    float pore_thr = 160.;
    size_t pore_cl_dist = 4000; // TODO maybe use frequency * 1sec here?
    //FIXME see if it ever helps!
    float relaxed_pore_thr = 150.;
    //usually template read region to the left of potential spacer region
    //FIXME rename to end_flank?!!
    size_t query_flank = 1200;
    //trim potentially erroneous (and/or PCR adapter) bases at end of query
    size_t query_trim = 200;
    //adjusted to adapter presense and potential loss of bases on query, leading to 'shift'
    //FIXME rename to start_flank?!!
    size_t target_flank = 1700;
    //FIXME should probably be fractional
    //currently has to account for the adapter
    int flank_edist = 150;
    //FIXME do we need both of them?
    int relaxed_flank_edist = 250;
    int adapter_edist = 4;
    int relaxed_adapter_edist = 6;
    uint64_t pore_adapter_range = 100; //bp TODO figure out good threshold
    //in bases
    uint64_t expect_adapter_prefix = 200;
    //in samples
    uint64_t expect_pore_prefix = 5000;
    int middle_adapter_search_span = 1000;

    //TAIL_ADAPTER = 'GCAATACGTAACTGAACGAAGT'
    //HEAD_ADAPTER = 'AATGTACTTCGTTCAGTTACGTATTGCT'
    //clipped 4 letters from the beginning of head adapter (24 left)
    std::string adapter = "TACTTCGTTCAGTTACGTATTGCT";

    explicit DuplexSplitSettings(bool simplex_mode = false) : simplex_mode(simplex_mode) {}
};

//TODO consider precumputing and reusing ranges with high signal
struct ExtRead {
    std::shared_ptr<Read> read;
    torch::Tensor data_as_float32;
    std::vector<uint64_t> move_sums;

    explicit ExtRead(std::shared_ptr<Read> r);
};

class DuplexSplitNode : public MessageSink {
public:
    typedef std::pair<uint64_t, uint64_t> PosRange;
    typedef std::vector<PosRange> PosRanges;
    typedef std::function<PosRanges (const ExtRead&)> SplitFinderF;

    DuplexSplitNode(MessageSink& sink, DuplexSplitSettings settings,
                    int num_worker_threads = 5, size_t max_reads = 1000);
    ~DuplexSplitNode();

private:
    std::vector<PosRange> possible_pore_regions(const ExtRead& read, float pore_thr) const;
    bool check_nearby_adapter(const Read& read, PosRange r, int adapter_edist) const;
    bool check_flank_match(const Read& read, PosRange r, int dist_thr) const;
    std::optional<PosRange> identify_extra_middle_split(const Read& read) const;

    std::vector<std::shared_ptr<Read>>
    split(std::shared_ptr<Read> read, const PosRanges& spacers) const;

    std::vector<std::pair<std::string, SplitFinderF>>
    build_split_finders() const;

    void worker_thread();  // Worker thread performs scaling and trimming asynchronously.
    MessageSink& m_sink;  // MessageSink to consume scaled reads.

    const DuplexSplitSettings m_settings;
    std::vector<std::pair<std::string, SplitFinderF>> m_split_finders;
    const int m_num_worker_threads;
    std::vector<std::unique_ptr<std::thread>> worker_threads;
};

}  // namespace dorado