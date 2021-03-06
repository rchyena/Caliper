// Copyright (c) 2017, Lawrence Livermore National Security, LLC.
// Produced at the Lawrence Livermore National Laboratory.
//
// This file is part of Caliper.
// Written by David Boehme, boehme3@llnl.gov.
// LLNL-CODE-678900
// All rights reserved.
//
// For details, see https://github.com/scalability-llnl/Caliper.
// Please also see the LICENSE file for our additional BSD notice.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
//  * Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the disclaimer below.
//  * Redistributions in binary form must reproduce the above copyright notice, this list of
//    conditions and the disclaimer (as noted below) in the documentation and/or other materials
//    provided with the distribution.
//  * Neither the name of the LLNS/LLNL nor the names of its contributors may be used to endorse
//    or promote products derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS
// OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// LAWRENCE LIVERMORE NATIONAL SECURITY, LLC, THE U.S. DEPARTMENT OF ENERGY OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

/// \file TreeFormatter.cpp
/// Pretty-print tree-organized snapshots 

#include "caliper/reader/TreeFormatter.h"

#include "caliper/reader/QuerySpec.h"
#include "caliper/reader/SnapshotTree.h"

#include "caliper/common/CaliperMetadataAccessInterface.h"

#include "caliper/common/Attribute.h"
#include "caliper/common/Node.h"

#include "caliper/common/util/split.hpp"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <mutex>
#include <utility>

using namespace cali;

namespace
{

const char whitespace[120+1] =
    "                                        "
    "                                        "
    "                                        ";

inline std::ostream& pad_right(std::ostream& os, const std::string& str, std::size_t width)
{
    os << str << whitespace+(120-std::min<std::size_t>(120, 1+width-str.size()));
    return os;
}

inline std::ostream& pad_left (std::ostream& os, const std::string& str, std::size_t width)
{
    os << whitespace+(120 - std::min<std::size_t>(120, width-str.size())) << str << ' ';
    return os;
}

} // namespace [anonymous]


struct TreeFormatter::TreeFormatterImpl
{
    SnapshotTree             m_tree;

    QuerySpec::AttributeSelection m_attribute_columns;
    std::map<Attribute, int> m_attribute_column_widths;

    int                      m_path_column_width;

    std::vector<std::string> m_path_key_names;
    std::vector<Attribute>   m_path_keys;

    std::mutex               m_path_key_lock;


    void configure(const QuerySpec& spec) {
        // set path keys (first argument in spec.format.args)
        if (spec.format.args.size() > 0)
            util::split(spec.format.args.front(), ',',
                        std::back_inserter(m_path_key_names));

        m_path_keys.assign(m_path_key_names.size(), Attribute::invalid);
        m_attribute_columns = spec.attribute_selection;
    }
    
    std::vector<Attribute> get_path_keys(const CaliperMetadataAccessInterface& db) {
        std::vector<Attribute> path_keys;

        {
            std::lock_guard<std::mutex>
                g(m_path_key_lock);

            path_keys = m_path_keys;
        }

        for (std::vector<Attribute>::size_type i = 0; i < path_keys.size(); ++i) 
            if (path_keys[i] == Attribute::invalid) {
                Attribute attr = db.get_attribute(m_path_key_names[i]);

                if (attr != Attribute::invalid) {
                    path_keys[i]   = attr;
                    std::lock_guard<std::mutex>
                        g(m_path_key_lock);
                    m_path_keys[i] = attr;
                } 
            }

        return path_keys;
    }

    void add(const CaliperMetadataAccessInterface& db, const EntryList& list) {
        const SnapshotTreeNode* node = nullptr;

        if (m_path_key_names.empty()) {
            node = m_tree.add_snapshot(db, list, [](const Attribute& attr,const Variant&){
                    return attr.is_nested();
                });
        } else { 
            auto path_keys = get_path_keys(db);

            node = m_tree.add_snapshot(db, list, [&path_keys](const Attribute& attr, const Variant&){
                    return (std::find(std::begin(path_keys), std::end(path_keys), 
                                      attr) != std::end(path_keys));
                });
        }
        
        if (!node)
            return;

        // update column widths

        {
            int len = node->label_value().to_string().size();

            for (const SnapshotTreeNode* n = node; n && n->label_key() != Attribute::invalid; n = n->parent())
                len += 2;

            m_path_column_width = std::max(m_path_column_width, len);
        }

        for (auto &p : node->attributes()) {
            int len = p.second.to_string().size();
            auto it = m_attribute_column_widths.find(p.first);

            if (it == m_attribute_column_widths.end())
                m_attribute_column_widths.insert(std::make_pair(p.first, std::max<int>(len, p.first.name().size())));
            else
                it->second = std::max(it->second, len);
        }
    }

    void recursive_print_nodes(const SnapshotTreeNode* node, 
                               int level, 
                               const std::vector<Attribute>& attributes, 
                               std::ostream& os)
    {
        // 
        // print this node
        //

        std::string path_str;
        path_str.assign(2*level, ' ');
        path_str.append(node->label_value().to_string());

        ::pad_right(os, path_str, m_path_column_width);

        for (const Attribute& a : attributes) {
            std::string str;

            {
                auto it = node->attributes().find(a);
                if (it != node->attributes().end())
                    str = it->second.to_string();
            }

            cali_attr_type t = a.type();
            bool align_right = (t == CALI_TYPE_INT    ||
                                t == CALI_TYPE_UINT   ||
                                t == CALI_TYPE_DOUBLE ||
                                t == CALI_TYPE_ADDR);

            if (align_right)
                ::pad_left (os, str, m_attribute_column_widths[a]);
            else
                ::pad_right(os, str, m_attribute_column_widths[a]);
        }

        os << std::endl;

        // 
        // recursively descend
        //

        for (node = node->first_child(); node; node = node->next_sibling())
            recursive_print_nodes(node, level+1, attributes, os);
    }

    void flush(const CaliperMetadataAccessInterface& db, std::ostream& os) {
        m_path_column_width = std::max<std::size_t>(m_path_column_width, 4 /* strlen("Path") */);

        //
        // establish order of attribute columns
        //

        std::vector<Attribute> attributes;

        switch (m_attribute_columns.selection) {
        case QuerySpec::AttributeSelection::Default:
            // auto-attributes: skip hidden and "cali." attributes
            for (auto &p : m_attribute_column_widths) {
                if (p.first.is_hidden())
                    continue;
                if (p.first.name().compare(0, 5, "cali.") == 0)
                    continue;

                attributes.push_back(p.first);
            }
            break;
        case QuerySpec::AttributeSelection::All:
            for (auto &p : m_attribute_column_widths)
                attributes.push_back(p.first);
            break;
        case QuerySpec::AttributeSelection::List:
            for (const std::string& s : m_attribute_columns.list) {
                Attribute attr = db.get_attribute(s);

                if (attr == Attribute::invalid)
                    std::cerr << "cali-query: TreeFormatter: Attribute \"" << s << "\" not found."
                              << std::endl;
                else
                    attributes.push_back(attr);
            }
            break;
        case QuerySpec::AttributeSelection::None:
            // keep empty list
            break;
        }

        //
        // print header
        //

        ::pad_right(os, "Path", m_path_column_width);

        for (const Attribute& a : attributes)
            ::pad_right(os, a.name(), m_attribute_column_widths[a]);

        os << std::endl;

        //
        // print tree nodes
        //

        const SnapshotTreeNode* node = m_tree.root();

        if (node)
            for (node = node->first_child(); node; node = node->next_sibling())
                recursive_print_nodes(node, 0, attributes, os);
    }

    TreeFormatterImpl()
        : m_path_column_width(0)
    { }
};


TreeFormatter::TreeFormatter(const QuerySpec& spec)
    : mP { new TreeFormatterImpl }
{
    mP->configure(spec);
}

TreeFormatter::~TreeFormatter()
{
    mP.reset();
}

void
TreeFormatter::process_record(CaliperMetadataAccessInterface& db, const EntryList& list)
{
    mP->add(db, list);
}

void
TreeFormatter::flush(CaliperMetadataAccessInterface& db, std::ostream& os)
{
    mP->flush(db, os);
}
