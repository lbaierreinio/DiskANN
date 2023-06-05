// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include "in_mem_graph_store.h"
#include "utils.h"

namespace diskann
{

InMemGraphStore::InMemGraphStore(const size_t max_pts) : AbstractGraphStore(max_pts)
{
}

int InMemGraphStore::load(const std::string &index_path_prefix)
{
    diskann::cout<<"Loading " << index_path_prefix << std::endl;
    return 0;
}
int InMemGraphStore::store(const std::string &index_path_prefix)
{
    diskann::cout<<"Storing to " << index_path_prefix << std::endl;    
    return 0;
}

void InMemGraphStore::get_adj_list(const location_t i, std::vector<location_t> &neighbors)
{
    diskann::cout<<i <<" " << neighbors.size() << std::endl;
}

void InMemGraphStore::set_adj_list(const location_t i, std::vector<location_t> &neighbors)
{
    diskann::cout<<i <<" " << neighbors.size() << std::endl;
}

} // namespace diskann
