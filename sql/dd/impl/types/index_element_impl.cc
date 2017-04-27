/* Copyright (c) 2014, 2017, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#include "dd/impl/types/index_element_impl.h"

#include <ostream>

#include "dd/impl/raw/raw_record.h"             // Raw_record
#include "dd/impl/sdi_impl.h"                   // sdi read/write functions
#include "dd/impl/tables/index_column_usage.h"  // Index_column_usage
#include "dd/impl/transaction_impl.h"           // Open_dictionary_tables_ctx
#include "dd/impl/types/entity_object_impl.h"
#include "dd/impl/types/table_impl.h"           // Table_impl
#include "dd/types/column.h"                    // Column
#include "dd/types/object_table.h"
#include "dd/types/weak_object.h"
#include "dd_table_share.h"                     // dd_get_old_field_type()
#include "m_string.h"
#include "my_inttypes.h"
#include "my_sys.h"
#include "mysqld_error.h"                       // ER_*
#include "rapidjson/document.h"
#include "rapidjson/prettywriter.h"

namespace dd {
class Object_key;
class Sdi_rcontext;
class Sdi_wcontext;
}  // namespace dd

using dd::tables::Index_column_usage;

namespace dd {

///////////////////////////////////////////////////////////////////////////
// Index_element implementation.
///////////////////////////////////////////////////////////////////////////

const Object_table &Index_element::OBJECT_TABLE()
{
  return Index_column_usage::instance();
}

///////////////////////////////////////////////////////////////////////////

const Object_type &Index_element::TYPE()
{
  static Index_element_type s_instance;
  return s_instance;
}

///////////////////////////////////////////////////////////////////////////
// Index_element_impl implementation.
///////////////////////////////////////////////////////////////////////////

bool Index_element_impl::validate() const
{
  if (!m_index)
  {
    my_error(ER_INVALID_DD_OBJECT,
             MYF(0),
             Index_element_impl::OBJECT_TABLE().name().c_str(),
             "No index object associated with this element.");
    return true;
  }

  return false;
}

///////////////////////////////////////////////////////////////////////////

bool Index_element_impl::restore_attributes(const Raw_record &r)
{
  // Must resolve ambiguity by static cast.
  if (check_parent_consistency(static_cast<Entity_object_impl*>(m_index),
        r.read_ref_id(Index_column_usage::FIELD_INDEX_ID)))
    return true;

  m_ordinal_position= r.read_uint(Index_column_usage::FIELD_ORDINAL_POSITION);

  m_order= (enum_index_element_order) r.read_int(Index_column_usage::FIELD_ORDER);

  m_column=
    m_index->table_impl().get_column(
      r.read_ref_id(
        Index_column_usage::FIELD_COLUMN_ID));

  m_length= r.read_uint(Index_column_usage::FIELD_LENGTH, (uint) -1);

  m_hidden= r.read_bool(Index_column_usage::FIELD_HIDDEN);

  return (m_column == NULL);
}

///////////////////////////////////////////////////////////////////////////

bool Index_element_impl::store_attributes(Raw_record *r)
{
  //
  // Special cases dealing with NULL values for nullable fields
  //  - store NULL if length is not set.
  //

  return r->store(Index_column_usage::FIELD_INDEX_ID, m_index->id()) ||
         r->store(Index_column_usage::FIELD_ORDINAL_POSITION, m_ordinal_position) ||
         r->store(Index_column_usage::FIELD_COLUMN_ID, m_column->id()) ||
         r->store(Index_column_usage::FIELD_LENGTH, m_length, m_length == (uint) -1) ||
         r->store(Index_column_usage::FIELD_HIDDEN, m_hidden) ||
         r->store(Index_column_usage::FIELD_ORDER, m_order);
}

///////////////////////////////////////////////////////////////////////////

static_assert(Index_column_usage::FIELD_HIDDEN==5,
              "Index_column_usage definition has changed, review (de)ser memfuns!");
void
Index_element_impl::serialize(Sdi_wcontext*, Sdi_writer *w) const
{
  w->StartObject();
  write(w, m_ordinal_position, STRING_WITH_LEN("ordinal_position"));
  write(w, m_length, STRING_WITH_LEN("length"));
  write_enum(w, m_order, STRING_WITH_LEN("order"));
  write_opx_reference(w, m_column,  STRING_WITH_LEN("column_opx"));
  w->EndObject();
}

///////////////////////////////////////////////////////////////////////////

bool
Index_element_impl::deserialize(Sdi_rcontext *rctx, const RJ_Value &val)
{
  read(&m_ordinal_position, val, "ordinal_position");
  read(&m_length, val, "length");
  read_enum(&m_order, val, "order");
  read_opx_reference(rctx, &m_column, val, "column_opx");
  return false;
}

///////////////////////////////////////////////////////////////////////////

void Index_element_impl::debug_print(String_type &outb) const
{
  dd::Stringstream_type ss;
  ss
    << "INDEX ELEMENT OBJECT: { "
    << "m_index: {OID: " << m_index->id() << "}; "
    << "m_column_id: {OID: " << m_column->id() << "}; "
    << "m_ordinal_position: " << m_ordinal_position << "; "
    << "m_length: " << m_length << "; "
    << "m_order: " << m_order << "; "
    << "m_hidden: " << m_hidden;

  ss << " }";

  outb= ss.str();
}

///////////////////////////////////////////////////////////////////////////

Object_key *Index_element_impl::create_primary_key() const
{
  return Index_column_usage::create_primary_key(
    m_index->id(), m_ordinal_position);
}

bool Index_element_impl::has_new_primary_key() const
{
  return m_index->has_new_primary_key();
}

///////////////////////////////////////////////////////////////////////////

/**
  Check if index element represents prefix key part on the column.

  @note This function is in sync with how we evaluate HA_PART_KEY_SEG.
        As result it returns funny results for BLOB/GIS types.
*/

bool Index_element_impl::is_prefix() const
{
  uint interval_parts;
  const Column& col= column();
  enum_field_types field_type= dd_get_old_field_type(col.type());

  if (field_type == MYSQL_TYPE_ENUM || field_type == MYSQL_TYPE_SET)
    interval_parts= col.elements_count();
  else
    interval_parts= 0;

  return calc_key_length(field_type,
                         col.char_length(),
                         col.numeric_scale(),
                         col.is_unsigned(),
                         interval_parts) != length();
}

///////////////////////////////////////////////////////////////////////////

Index_element_impl *Index_element_impl::clone(const Index_element_impl &other,
                                              Index_impl *index)
{
  return new Index_element_impl(other, index,
                                index->table_impl().get_column(other.column().name()));
}

Index_element_impl::Index_element_impl(const Index_element_impl &src,
                                       Index_impl *parent, Column *column)
  : Weak_object(src),
    m_ordinal_position(src.m_ordinal_position), m_length(src.m_length),
    m_order(src.m_order), m_hidden(src.m_hidden),
    m_index(parent),
    m_column(column)
{}

///////////////////////////////////////////////////////////////////////////
//Index_element_type implementation.
///////////////////////////////////////////////////////////////////////////

void Index_element_type::register_tables(Open_dictionary_tables_ctx *otx) const
{
  otx->add_table<Index_column_usage>();
}

///////////////////////////////////////////////////////////////////////////

}
