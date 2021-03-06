////////////////////////////////////////////////////////////////////////////
//
// Copyright 2016 Realm Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
////////////////////////////////////////////////////////////////////////////

#ifndef REALM_OS_OBJECT_ACCESSOR_HPP
#define REALM_OS_OBJECT_ACCESSOR_HPP

#include "object.hpp"

#include "feature_checks.hpp"
#include "list.hpp"
#include "object_schema.hpp"
#include "object_store.hpp"
#include "results.hpp"
#include "schema.hpp"
#include "shared_realm.hpp"

#include <realm/util/assert.hpp>
#include <realm/table_view.hpp>

#if REALM_ENABLE_SYNC
#include <realm/sync/object.hpp>
#endif // REALM_ENABLE_SYNC

#include <string>

namespace realm {
template <typename ValueType, typename ContextType>
void Object::set_property_value(ContextType& ctx, StringData prop_name,
                                ValueType value, CreatePolicy policy)
{
    verify_attached();
    m_realm->verify_in_write();
    auto& property = property_for_name(prop_name);

    // Modifying primary keys is allowed in migrations to make it possible to
    // add a new primary key to a type (or change the property type), but it
    // is otherwise considered the immutable identity of the row
    if (property.is_primary && !m_realm->is_in_migration())
        throw ModifyPrimaryKeyException(m_object_schema->name, property.name);

    set_property_value_impl(ctx, property, value, policy, false);
}

template <typename ValueType, typename ContextType>
ValueType Object::get_property_value(ContextType& ctx, const Property& property) const
{
    return get_property_value_impl<ValueType>(ctx, property);
}

template <typename ValueType, typename ContextType>
ValueType Object::get_property_value(ContextType& ctx, StringData prop_name) const
{
    return get_property_value_impl<ValueType>(ctx, property_for_name(prop_name));
}

namespace {
template <typename ValueType, typename ContextType>
struct ValueUpdater {
    ContextType& ctx;
    Property const& property;
    ValueType& value;
    Obj& obj;
    ColKey col;
    CreatePolicy policy;
    bool is_default;

    void operator()(Obj*)
    {
        ContextType child_ctx(ctx, property);
        auto curr_link = obj.get<ObjKey>(col);
        auto link = child_ctx.template unbox<Obj>(value, policy, curr_link);
        if (policy != CreatePolicy::UpdateModified || curr_link != link.get_key()) {
            obj.set(col, link.get_key());
        }
    }

    template<typename T>
    void operator()(T*)
    {
        auto new_val = ctx.template unbox<T>(value);
        if (policy != CreatePolicy::UpdateModified || obj.get<T>(col) != new_val) {
            obj.set(col, new_val, is_default);
        }
    }
};
}

template <typename ValueType, typename ContextType>
void Object::set_property_value_impl(ContextType& ctx, const Property &property,
                                     ValueType value, CreatePolicy policy, bool is_default)
{
    ctx.will_change(*this, property);

    ColKey col{property.column_key};
    if (is_nullable(property.type) && ctx.is_null(value)) {
        if (policy != CreatePolicy::UpdateModified || !m_obj.is_null(col)) {
            if (property.type == PropertyType::Object) {
                if (!is_default)
                    m_obj.set_null(col);
            }
            else {
                m_obj.set_null(col, is_default);
            }
        }

        ctx.did_change();
        return;
    }

    if (is_array(property.type)) {
        if (property.type == PropertyType::LinkingObjects)
            throw ReadOnlyPropertyException(m_object_schema->name, property.name);

        ContextType child_ctx(ctx, property);
        List list(m_realm, m_obj, col);
        list.assign(child_ctx, value, policy);
        ctx.did_change();
        return;
    }

    ValueUpdater<ValueType, ContextType> updater{ctx, property, value,
        m_obj, col, policy, is_default};
    switch_on_type(property.type, updater);
    ctx.did_change();
}

template <typename ValueType, typename ContextType>
ValueType Object::get_property_value_impl(ContextType& ctx, const Property &property) const
{
    verify_attached();

    ColKey column{property.column_key};
    if (is_nullable(property.type) && m_obj.is_null(column))
        return ctx.null_value();
    if (is_array(property.type) && property.type != PropertyType::LinkingObjects)
        return ctx.box(List(m_realm, m_obj, column));

    switch (property.type & ~PropertyType::Flags) {
        case PropertyType::Bool:   return ctx.box(m_obj.get<bool>(column));
        case PropertyType::Int:    return is_nullable(property.type) ? ctx.box(*m_obj.get<util::Optional<int64_t>>(column)) : ctx.box(m_obj.get<int64_t>(column));
        case PropertyType::Float:  return ctx.box(m_obj.get<float>(column));
        case PropertyType::Double: return ctx.box(m_obj.get<double>(column));
        case PropertyType::String: return ctx.box(m_obj.get<StringData>(column));
        case PropertyType::Data:   return ctx.box(m_obj.get<BinaryData>(column));
        case PropertyType::Date:   return ctx.box(m_obj.get<Timestamp>(column));
//        case PropertyType::Any:    return ctx.box(m_obj.get<Mixed>(column));
        case PropertyType::Object: {
            auto linkObjectSchema = m_realm->schema().find(property.object_type);
            return ctx.box(Object(m_realm, *linkObjectSchema,
                                  const_cast<Obj&>(m_obj).get_linked_object(column)));
        }
        case PropertyType::LinkingObjects: {
            auto target_object_schema = m_realm->schema().find(property.object_type);
            auto link_property = target_object_schema->property_for_name(property.link_origin_property_name);
            auto table = m_realm->read_group().get_table(target_object_schema->table_key);
            auto tv = const_cast<Obj&>(m_obj).get_backlink_view(table, ColKey(link_property->column_key));
            return ctx.box(Results(m_realm, std::move(tv)));
        }
        default: REALM_UNREACHABLE();
    }
}

template<typename ValueType, typename ContextType>
Object Object::create(ContextType& ctx, std::shared_ptr<Realm> const& realm,
                      StringData object_type, ValueType value,
                      CreatePolicy policy, ObjKey current_obj, Obj* out_row)
{
    auto object_schema = realm->schema().find(object_type);
    REALM_ASSERT(object_schema != realm->schema().end());
    return create(ctx, realm, *object_schema, value, policy, current_obj, out_row);
}

template<typename ValueType, typename ContextType>
Object Object::create(ContextType& ctx, std::shared_ptr<Realm> const& realm,
                      ObjectSchema const& object_schema, ValueType value,
                      CreatePolicy policy, ObjKey current_obj, Obj* out_row)
{
    realm->verify_in_write();

    // get or create our accessor
    bool created = false;

    // try to get existing row if updating
    Obj obj;
    auto table = realm->read_group().get_table(object_schema.table_key);

    bool skip_primary = true;
    if (auto primary_prop = object_schema.primary_key_property()) {
        // search for existing object based on primary key type
        auto primary_value = ctx.value_for_property(value, *primary_prop,
                                                    primary_prop - &object_schema.persisted_properties[0]);
        if (!primary_value)
            primary_value = ctx.default_value_for_property(object_schema, *primary_prop);
        if (!primary_value) {
            if (!is_nullable(primary_prop->type))
                throw MissingPropertyValueException(object_schema.name, primary_prop->name);
            primary_value = ctx.null_value();
        }
        auto key = get_for_primary_key_impl(ctx, *table, *primary_prop, *primary_value);
        if (key) {
            if (policy != CreatePolicy::ForceCreate)
                obj = table->get_object(key);
            else if (realm->is_in_migration()) {
                // Creating objects with duplicate primary keys is allowed in migrations
                // as long as there are no duplicates at the end, as adding an entirely
                // new column which is the PK will inherently result in duplicates at first
                obj = table->create_object();
                created = true;
                skip_primary = false;
            }
            else {
                throw std::logic_error(util::format("Attempting to create an object of type '%1' with an existing primary key value '%2'.",
                                                    object_schema.name, ctx.print(*primary_value)));
            }
        }
        else {
            created = true;
            Mixed primary_key;
            if (primary_prop->type == PropertyType::Int) {
                primary_key = ctx.template unbox<util::Optional<int64_t>>(*primary_value);
            }
            else if (primary_prop->type == PropertyType::String) {
                primary_key = ctx.template unbox<StringData>(*primary_value);
            }
            else {
                REALM_TERMINATE("Unsupported primary key type.");
            }
            obj = table->create_object_with_primary_key(primary_key);
        }
    }
    else {
        if (policy == CreatePolicy::UpdateModified && current_obj) {
            obj = table->get_object(current_obj);
        }
        else {
        obj = table->create_object();
            created = true;
        }
    }

    // populate
    Object object(realm, object_schema, obj);
    if (out_row)
        *out_row = obj;
    for (size_t i = 0; i < object_schema.persisted_properties.size(); ++i) {
        auto& prop = object_schema.persisted_properties[i];
        if (skip_primary && prop.is_primary)
            continue;

        auto v = ctx.value_for_property(value, prop, i);
        if (!created && !v)
            continue;

        bool is_default = false;
        if (!v) {
            v = ctx.default_value_for_property(object_schema, prop);
            is_default = true;
        }
        if ((!v || ctx.is_null(*v)) && !is_nullable(prop.type) && !is_array(prop.type)) {
            if (prop.is_primary || !ctx.allow_missing(value))
                throw MissingPropertyValueException(object_schema.name, prop.name);
        }
        if (v)
            object.set_property_value_impl(ctx, prop, *v, policy, is_default);
    }
#if REALM_ENABLE_SYNC
    if (realm->is_partial() && object_schema.name == "__User") {
        object.ensure_user_in_everyone_role();
        object.ensure_private_role_exists_for_user();
    }
#endif
    return object;
}

template<typename ValueType, typename ContextType>
Object Object::get_for_primary_key(ContextType& ctx, std::shared_ptr<Realm> const& realm,
                      StringData object_type, ValueType primary_value)
{
    auto object_schema = realm->schema().find(object_type);
    REALM_ASSERT(object_schema != realm->schema().end());
    return get_for_primary_key(ctx, realm, *object_schema, primary_value);
}

template<typename ValueType, typename ContextType>
Object Object::get_for_primary_key(ContextType& ctx, std::shared_ptr<Realm> const& realm,
                                   const ObjectSchema &object_schema,
                                   ValueType primary_value)
{
    auto primary_prop = object_schema.primary_key_property();
    if (!primary_prop) {
        throw MissingPrimaryKeyException(object_schema.name);
    }

    TableRef table;
    if (object_schema.table_key)
        table = realm->read_group().get_table(object_schema.table_key);
    if (!table)
        return Object(realm, object_schema, Obj());
    auto key = get_for_primary_key_impl(ctx, *table, *primary_prop, primary_value);
    return Object(realm, object_schema, key ? table->get_object(key) : Obj{});
}

template<typename ValueType, typename ContextType>
ObjKey Object::get_for_primary_key_impl(ContextType& ctx, Table const& table,
                                        const Property &primary_prop,
                                        ValueType primary_value) {
    bool is_null = ctx.is_null(primary_value);
    if (is_null && !is_nullable(primary_prop.type))
        throw std::logic_error("Invalid null value for non-nullable primary key.");
    if (primary_prop.type == PropertyType::String) {
        return table.find_first(primary_prop.column_key,
                                ctx.template unbox<StringData>(primary_value));
    }
    if (is_nullable(primary_prop.type))
        return table.find_first(primary_prop.column_key,
                                ctx.template unbox<util::Optional<int64_t>>(primary_value));
    return table.find_first(primary_prop.column_key,
                            ctx.template unbox<int64_t>(primary_value));
}

} // namespace realm

#endif // REALM_OS_OBJECT_ACCESSOR_HPP
