// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include <index.h>
#include <numeric>
#include <omp.h>
#include <string.h>
#include <time.h>
#include <timer.h>
#include <boost/program_options.hpp>
#include <future>

#include "utils.h"
#include "filter_utils.h"
#include "program_options_utils.hpp"
#include "index_factory.h"

#ifndef _WINDOWS
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "memory_mapper.h"

namespace po = boost::program_options;

// load_aligned_bin modified to read pieces of the file, but using ifstream
// instead of cached_ifstream.
template <typename T>
inline void load_aligned_bin_part(const std::string &bin_file, T *data, size_t offset_points, size_t points_to_read)
{
    diskann::Timer timer;
    std::ifstream reader;
    reader.exceptions(std::ios::failbit | std::ios::badbit);
    reader.open(bin_file, std::ios::binary | std::ios::ate);
    size_t actual_file_size = reader.tellg();
    reader.seekg(0, std::ios::beg);

    int npts_i32, dim_i32;
    reader.read((char *)&npts_i32, sizeof(int));
    reader.read((char *)&dim_i32, sizeof(int));
    size_t npts = (uint32_t)npts_i32;
    size_t dim = (uint32_t)dim_i32;

    size_t expected_actual_file_size = npts * dim * sizeof(T) + 2 * sizeof(uint32_t);
    if (actual_file_size != expected_actual_file_size)
    {
        std::stringstream stream;
        stream << "Error. File size mismatch. Actual size is " << actual_file_size << " while expected size is  "
               << expected_actual_file_size << " npts = " << npts << " dim = " << dim << " size of <T>= " << sizeof(T)
               << std::endl;
        std::cout << stream.str();
        throw diskann::ANNException(stream.str(), -1, __FUNCSIG__, __FILE__, __LINE__);
    }

    if (offset_points + points_to_read > npts)
    {
        std::stringstream stream;
        stream << "Error. Not enough points in file. Requested " << offset_points << "  offset and " << points_to_read
               << " points, but have only " << npts << " points" << std::endl;
        std::cout << stream.str();
        throw diskann::ANNException(stream.str(), -1, __FUNCSIG__, __FILE__, __LINE__);
    }

    reader.seekg(2 * sizeof(uint32_t) + offset_points * dim * sizeof(T));

    const size_t rounded_dim = ROUND_UP(dim, 8);

    for (size_t i = 0; i < points_to_read; i++)
    {
        reader.read((char *)(data + i * rounded_dim), dim * sizeof(T));
        memset(data + i * rounded_dim + dim, 0, (rounded_dim - dim) * sizeof(T));
    }
    reader.close();

    const double elapsedSeconds = timer.elapsed() / 1000000.0;
    std::cout << "Read " << points_to_read << " points using non-cached reads in " << elapsedSeconds << std::endl;
}

std::string get_save_filename(const std::string &save_path, size_t points_to_skip, size_t points_deleted,
                              size_t last_point_threshold)
{
    std::string final_path = save_path;
    if (points_to_skip > 0)
    {
        final_path += "skip" + std::to_string(points_to_skip) + "-";
    }

    final_path += "del" + std::to_string(points_deleted) + "-";
    final_path += std::to_string(last_point_threshold);
    return final_path;
}

template <typename T, typename TagT>
void delete_from_beginning(diskann::AbstractIndex &index, diskann::IndexWriteParameters &delete_params,
                           size_t points_to_skip, size_t points_to_delete_from_beginning)
{
    try
    {
        std::cout << std::endl
                  << "Lazy deleting points " << points_to_skip << " to "
                  << points_to_skip + points_to_delete_from_beginning << "... ";
        for (size_t i = points_to_skip; i < points_to_skip + points_to_delete_from_beginning; ++i)
            index.lazy_delete(static_cast<TagT>(i + 1)); // Since tags are data location + 1
        std::cout << "done." << std::endl;

        auto report = index.consolidate_deletes(delete_params);
        std::cout << "#active points: " << report._active_points << std::endl
                  << "max points: " << report._max_points << std::endl
                  << "empty slots: " << report._empty_slots << std::endl
                  << "deletes processed: " << report._slots_released << std::endl
                  << "latest delete size: " << report._delete_set_size << std::endl
                  << "rate: (" << points_to_delete_from_beginning / report._time << " points/second overall, "
                  << points_to_delete_from_beginning / report._time / delete_params.num_threads << " per thread)"
                  << std::endl;
    }
    catch (std::system_error &e)
    {
        std::cout << "Exception caught in deletion thread: " << e.what() << std::endl;
    }
}

template <typename T>
void build_incremental_index(const std::string &data_path, diskann::IndexWriteParameters &params, const std::string &index_path,
                            bool concurrent, const std::string &label_file, uint32_t num_threads, uint32_t L,
                             const std::string &universal_label)
{
    size_t dim, aligned_dim;
    size_t num_points;
    diskann::get_bin_metadata(data_path, num_points, dim);
    aligned_dim = ROUND_UP(dim, 8);
    bool has_labels = label_file != "";
    using TagT = uint32_t;
    using LabelT = uint32_t;

    bool enable_tags = true;
    using TagT = uint32_t;
    auto index_search_params = diskann::IndexSearchParams(params.search_list_size, params.num_threads);
    diskann::IndexConfig index_config = diskann::IndexConfigBuilder()
                                            .with_metric(diskann::L2)
                                            .with_dimension(dim)
                                            .with_max_points(num_points)
                                            .is_dynamic_index(true)
                                            .with_index_write_params(params)
                                            .with_index_search_params(index_search_params)
                                            .with_data_type(diskann_type_to_name<T>())
                                            .with_tag_type(diskann_type_to_name<TagT>())
                                            .with_label_type(diskann_type_to_name<LabelT>())
                                            .with_data_load_store_strategy(diskann::DataStoreStrategy::MEMORY)
                                            .with_graph_load_store_strategy(diskann::GraphStoreStrategy::MEMORY)
                                            .is_enable_tags(enable_tags)
                                            .is_filtered(has_labels)
                                            .is_concurrent_consolidate(concurrent)
                                            .build();

    diskann::IndexFactory index_factory = diskann::IndexFactory(index_config);
    auto index = index_factory.create_instance();
    index->load(index_path.c_str(), num_threads, L); // TODO: Verify parameters
    std::cout << "Index loaded" << std::endl;

    if (universal_label != "")
    {
        LabelT u_label = 0;
        index->set_universal_label(u_label);
    }

    T *data = nullptr;
    diskann::alloc_aligned(
        (void **)&data, num_points * aligned_dim * sizeof(T), 8 * sizeof(T));

    std::vector<TagT> tags(num_points);
    std::iota(tags.begin(), tags.end(), 1 + static_cast<TagT>(0));

    load_aligned_bin_part(data_path, data, 0, num_points);

    std::string labels_file_to_use = index_path + "_label_formatted.txt";
    std::string mem_labels_int_map_file = index_path + "_labels_map.txt";
    if (has_labels)
    {
        convert_labels_string_to_int(label_file, labels_file_to_use, mem_labels_int_map_file, universal_label);
    }

    delete_from_beginning<T, TagT>(*index, params, 100, 1000);

    index->save(index_path.c_str(), true); // Save back
}

int main(int argc, char **argv)
{
    std::string data_type, dist_fn, data_path, index_path_prefix;
    uint32_t num_threads, R, L, num_start_pts;
    float alpha, start_point_norm;
    size_t points_to_skip, max_points_to_insert, beginning_index_size, points_per_checkpoint, checkpoints_per_snapshot,
        points_to_delete_from_beginning, start_deletes_after;
    bool concurrent;

    // label options
    std::string label_file, label_type, universal_label;
    std::uint32_t Lf, unique_labels_supported;

    po::options_description desc{program_options_utils::make_program_description("test_insert_deletes_consolidate",
                                                                                 "Test insert deletes & consolidate")};
    try
    {
        desc.add_options()("help,h", "Print information on arguments");

        // Required parameters
        po::options_description required_configs("Required");
        required_configs.add_options()("data_type", po::value<std::string>(&data_type)->required(),
                                       program_options_utils::DATA_TYPE_DESCRIPTION);
        required_configs.add_options()("dist_fn", po::value<std::string>(&dist_fn)->required(),
                                       program_options_utils::DISTANCE_FUNCTION_DESCRIPTION);
        required_configs.add_options()("index_path_prefix", po::value<std::string>(&index_path_prefix)->required(),
                                       program_options_utils::INDEX_PATH_PREFIX_DESCRIPTION);
        required_configs.add_options()("data_path", po::value<std::string>(&data_path)->required(),
                                       program_options_utils::INPUT_DATA_PATH);
        required_configs.add_options()("points_to_skip", po::value<uint64_t>(&points_to_skip)->required(),
                                       "Skip these first set of points from file");
        required_configs.add_options()("beginning_index_size", po::value<uint64_t>(&beginning_index_size)->required(),
                                       "Batch build will be called on these set of points");
        required_configs.add_options()("points_per_checkpoint", po::value<uint64_t>(&points_per_checkpoint)->required(),
                                       "Insertions are done in batches of points_per_checkpoint");
        required_configs.add_options()("checkpoints_per_snapshot",
                                       po::value<uint64_t>(&checkpoints_per_snapshot)->required(),
                                       "Save the index to disk every few checkpoints");
        required_configs.add_options()("points_to_delete_from_beginning",
                                       po::value<uint64_t>(&points_to_delete_from_beginning)->required(), "");

        // Optional parameters
        po::options_description optional_configs("Optional");
        optional_configs.add_options()("num_threads,T",
                                       po::value<uint32_t>(&num_threads)->default_value(omp_get_num_procs()),
                                       program_options_utils::NUMBER_THREADS_DESCRIPTION);
        optional_configs.add_options()("max_degree,R", po::value<uint32_t>(&R)->default_value(64),
                                       program_options_utils::MAX_BUILD_DEGREE);
        optional_configs.add_options()("Lbuild,L", po::value<uint32_t>(&L)->default_value(100),
                                       program_options_utils::GRAPH_BUILD_COMPLEXITY);
        optional_configs.add_options()("alpha", po::value<float>(&alpha)->default_value(1.2f),
                                       program_options_utils::GRAPH_BUILD_ALPHA);
        optional_configs.add_options()("max_points_to_insert",
                                       po::value<uint64_t>(&max_points_to_insert)->default_value(0),
                                       "These number of points from the file are inserted after "
                                       "points_to_skip");
        optional_configs.add_options()("do_concurrent", po::value<bool>(&concurrent)->default_value(false), "");
        optional_configs.add_options()("start_deletes_after",
                                       po::value<uint64_t>(&start_deletes_after)->default_value(0), "");
        optional_configs.add_options()("start_point_norm", po::value<float>(&start_point_norm)->default_value(0),
                                       "Set the start point to a random point on a sphere of this radius");

        // optional params for filters
        optional_configs.add_options()("label_file", po::value<std::string>(&label_file)->default_value(""),
                                       "Input label file in txt format for Filtered Index search. "
                                       "The file should contain comma separated filters for each node "
                                       "with each line corresponding to a graph node");
        optional_configs.add_options()("universal_label", po::value<std::string>(&universal_label)->default_value(""),
                                       "Universal label, if using it, only in conjunction with labels_file");
        optional_configs.add_options()("FilteredLbuild,Lf", po::value<uint32_t>(&Lf)->default_value(0),
                                       "Build complexity for filtered points, higher value "
                                       "results in better graphs");
        optional_configs.add_options()("label_type", po::value<std::string>(&label_type)->default_value("uint"),
                                       "Storage type of Labels <uint/ushort>, default value is uint which "
                                       "will consume memory 4 bytes per filter");
        optional_configs.add_options()("unique_labels_supported",
                                       po::value<uint32_t>(&unique_labels_supported)->default_value(0),
                                       "Number of unique labels supported by the dynamic index.");

        optional_configs.add_options()(
            "num_start_points",
            po::value<uint32_t>(&num_start_pts)->default_value(diskann::defaults::NUM_FROZEN_POINTS_DYNAMIC),
            "Set the number of random start (frozen) points to use when "
            "inserting and searching");

        // Merge required and optional parameters
        desc.add(required_configs).add(optional_configs);

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);
        if (vm.count("help"))
        {
            std::cout << desc;
            return 0;
        }
        po::notify(vm);
    }
    catch (const std::exception &ex)
    {
        std::cerr << ex.what() << '\n';
        return -1;
    }

    bool has_labels = false;
    if (!label_file.empty() || label_file != "")
    {
        has_labels = true;
    }

    if (num_start_pts < unique_labels_supported)
    {
        num_start_pts = unique_labels_supported;
    }

    try
    {
        diskann::IndexWriteParameters params = diskann::IndexWriteParametersBuilder(L, R)
                                                   .with_max_occlusion_size(500)
                                                   .with_alpha(alpha)
                                                   .with_num_threads(num_threads)
                                                   .with_filter_list_size(Lf)
                                                   .build();

        if (data_type == std::string("int8"))
            build_incremental_index<int8_t>(
                data_path, params, index_path_prefix, concurrent, label_file, num_threads, L, universal_label);
        else if (data_type == std::string("uint8"))
            build_incremental_index<uint8_t>(
                data_path, params, index_path_prefix, concurrent, label_file, num_threads, L, universal_label);
        else if (data_type == std::string("float"))
            build_incremental_index<float>(
                data_path, params, index_path_prefix, concurrent, label_file, num_threads, L, universal_label
            );
        else
            std::cout << "Unsupported type. Use float/int8/uint8" << std::endl;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Caught exception: " << e.what() << std::endl;
        exit(-1);
    }
    catch (...)
    {
        std::cerr << "Caught unknown exception" << std::endl;
        exit(-1);
    }

    return 0;
}
