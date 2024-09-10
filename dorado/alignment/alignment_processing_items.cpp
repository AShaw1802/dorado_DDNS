#include "alignment_processing_items.h"

#include "utils/PostCondition.h"
#include "utils/fastq_reader.h"
#include "utils/scoped_trace_log.h"
#include "utils/stream_utils.h"
#include "utils/tty_utils.h"
#include "utils/types.h"

#include <htslib/hts.h>
#include <htslib/sam.h>
#include <spdlog/spdlog.h>

#include <set>

namespace fs = std::filesystem;

namespace {
using OutputMode = dorado::utils::HtsFile::OutputMode;

OutputMode get_stdout_output_mode() {
    if (dorado::utils::is_fd_tty(stdout)) {
        return OutputMode::SAM;
    } else if (dorado::utils::is_fd_pipe(stdout)) {
        return OutputMode::UBAM;
    }
    return OutputMode::BAM;
}

std::set<std::string>& get_supported_compression_extensions() {
    static std::set<std::string> supported_compression_extensions{".gzip", ".gz"};
    return supported_compression_extensions;
};

bool is_loadable_by_htslib(const std::filesystem::path& input_path) {
    dorado::HtsFilePtr hts_file(hts_open(input_path.string().c_str(), "r"));
    if (!hts_file) {
        return false;
    }

    dorado::SamHdrPtr header(sam_hdr_read(hts_file.get()));
    return header != nullptr;
}

bool is_valid_input_file(const std::filesystem::path& input_path) {
    return is_loadable_by_htslib(input_path) || dorado::utils::is_fastq(input_path.string());
}

fs::path replace_extension(fs::path output_path) {
    while (get_supported_compression_extensions().count(output_path.extension().string())) {
        output_path.replace_extension();
    }
    return output_path.replace_extension("bam");
}

}  // namespace

namespace dorado::alignment {

AlignmentProcessingItems::AlignmentProcessingItems(std::string input_path,
                                                   bool recursive_input,
                                                   std::string output_folder,
                                                   bool allow_output_to_folder_from_stdin)
        : m_input_path(std::move(input_path)),
          m_output_folder(std::move(output_folder)),
          m_recursive_input(recursive_input),
          m_allow_output_to_folder_from_stdin(allow_output_to_folder_from_stdin) {}

bool AlignmentProcessingItems::check_recursive_arg_false() {
    if (!m_recursive_input) {
        return true;
    }
    spdlog::error("'--recursive is not valid unless input is from folder.");
    return false;
}

bool AlignmentProcessingItems::try_create_output_folder() {
    std::error_code creation_error;
    // N.B. No error code if folder already exists.
    fs::create_directories(fs::path{m_output_folder}, creation_error);
    if (creation_error) {
        spdlog::error("Unable to create output folder {}. ErrorCode({}) {}", m_output_folder,
                      creation_error.value(), creation_error.message());
        return false;
    }
    return true;
}

bool AlignmentProcessingItems::check_output_folder_for_input_folder(
        const std::string& input_folder) {
    // Don't allow inout and output folders to be the same, in order
    // to avoid any complexity associated with output overwriting input.
    auto absolute_input_path = fs::absolute(fs::path(input_folder));
    auto absolute_output_path = fs::absolute(fs::path(m_output_folder));

    if (absolute_input_path == absolute_output_path) {
        spdlog::error("Output folder may not be the same as the input folder");
        return false;
    }
    if (!try_create_output_folder()) {
        return false;
    }
    return true;
}

void AlignmentProcessingItems::add_to_working_files(
        const std::filesystem::path& input_relative_path) {
    auto output = replace_extension(fs::path(m_output_folder) / input_relative_path);

    m_working_paths[output.string()].push_back(input_relative_path);
}

bool AlignmentProcessingItems::try_add_to_working_files(const fs::path& input_root,
                                                        const fs::path& input_relative_path) {
    if (!is_valid_input_file(input_root / input_relative_path)) {
        return false;
    }

    add_to_working_files(input_relative_path);
    return true;
}

bool AlignmentProcessingItems::initialise_for_file() {
    if (!check_recursive_arg_false()) {
        return false;
    }

    if (m_output_folder.empty()) {
        // special handling, different output mode and output file is stdout indicator.
        if (!is_valid_input_file(fs::path{m_input_path})) {
            return false;
        }
        m_processing_list.emplace_back(m_input_path, "-", get_stdout_output_mode());
        return true;
    }

    auto input_file_path = fs::absolute(fs::path(m_input_path));

    if (!check_output_folder_for_input_folder(input_file_path.parent_path().string())) {
        return false;
    }

    if (!is_valid_input_file(input_file_path)) {
        return false;
    }

    auto output = replace_extension(fs::path(m_output_folder) / input_file_path.filename());
    m_processing_list.emplace_back(m_input_path, output.string(), OutputMode::SAM);

    return true;
}

template <class ITER>
void AlignmentProcessingItems::create_working_file_map() {
    utils::SuppressStderr stderr_suppressed{};
    const fs::path input_root(m_input_path);
    for (const fs::directory_entry& dir_entry : ITER(input_root)) {
        const auto& input_path = dir_entry.path();
        auto relative_path = fs::relative(input_path, input_root);
        try_add_to_working_files(input_root, relative_path);
    }
}

template <class ITER>
void AlignmentProcessingItems::add_all_valid_files() {
    create_working_file_map<ITER>();

    const fs::path input_root(m_input_path);
    const fs::path output_root(m_output_folder);
    for (const auto& output_to_inputs_pair : m_working_paths) {
        const auto& input_files = output_to_inputs_pair.second;
        if (input_files.size() == 1) {
            // single unique output file name
            const auto input = (input_root / input_files[0]).string();
            const auto& output = output_to_inputs_pair.first;
            m_processing_list.emplace_back(input, output, OutputMode::SAM);
        } else {
            // duplicate output names, disambiguate by preserving input file extension and extending with '.bam'
            for (const auto& input_relative_path : input_files) {
                const auto input = (input_root / input_relative_path).string();
                const auto output = (output_root / input_relative_path).string() + ".bam";
                m_processing_list.emplace_back(input, output, OutputMode::SAM);
            }
        }
    }
}

bool AlignmentProcessingItems::initialise_for_folder() {
    if (m_output_folder.empty()) {
        spdlog::error("An output-dir must be specified if reading from an input folder.");
        return false;
    }
    if (!check_output_folder_for_input_folder(m_input_path)) {
        return false;
    }

    if (m_recursive_input) {
        add_all_valid_files<fs::recursive_directory_iterator>();
    } else {
        add_all_valid_files<fs::directory_iterator>();
    }

    return true;
}

bool AlignmentProcessingItems::initialise_for_stdin() {
    if (!m_output_folder.empty() && !m_allow_output_to_folder_from_stdin) {
        spdlog::error("--output-dir is not valid if input is stdin.");
        return false;
    }
    if (!check_recursive_arg_false()) {
        return false;
    }
    m_processing_list.emplace_back("-", "-", get_stdout_output_mode());
    return true;
}

bool AlignmentProcessingItems::initialise() {
    auto trace = utils::ScopedTraceLog(std::string{"AlignmentProcessingItems::"} + __func__);
    if (m_input_path.empty()) {
        return initialise_for_stdin();
    }

    if (fs::is_directory(fs::path{m_input_path})) {
        return initialise_for_folder();
    }

    return initialise_for_file();
}

}  // namespace dorado::alignment
