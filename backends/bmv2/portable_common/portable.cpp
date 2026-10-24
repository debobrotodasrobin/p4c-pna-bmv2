/*
Copyright 2024 Marvell Technology, Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#include "portable.h"

#include "frontends/common/model.h"
#include "frontends/p4/cloner.h"

namespace P4::BMV2 {

using namespace P4::literals;

void PortableCodeGenerator::createStructLike(ConversionContext *ctxt, const IR::Type_StructLike *st,
                                             P4::PortableProgramStructure *structure) {
    CHECK_NULL(st);
    cstring name = st->controlPlaneName();
    unsigned max_length = 0;  // for variable-sized headers
    bool varbitFound = false;
    auto fields = new Util::JsonArray();
    LOG5("In createStructLike with struct " << st->toString());
    for (auto f : st->fields) {
        auto field = new Util::JsonArray();
        auto ftype = structure->typeMap->getType(f, true);
        LOG5("Iterating field with field " << f << " and type " << ftype->toString());
        if (ftype->to<IR::Type_StructLike>()) {
            BUG("%1%: nested structure", st);
        } else if (ftype->is<IR::Type_Boolean>()) {
            field->append(f->name.name);
            field->append(1);
            field->append(false);
            max_length += 1;
        } else if (auto type = ftype->to<IR::Type_Bits>()) {
            field->append(f->name.name);
            field->append(type->size);
            field->append(type->isSigned);
            max_length += type->size;
        } else if (auto type = ftype->to<IR::Type_Varbits>()) {
            field->append(f->name.name);
            max_length += type->size;
            field->append("*");
            if (varbitFound)
                ::P4::error(ErrorType::ERR_UNSUPPORTED,
                            "%1%: headers with multiple varbit fields are not supported", st);
            varbitFound = true;
        } else if (ftype->is<IR::Type_Error>()) {
            field->append(f->name.name);
            field->append(error_width);
            field->append(false);
            max_length += error_width;
        } else if (ftype->to<IR::Type_Array>()) {
            BUG("%1%: nested stack", st);
        } else {
            BUG("%1%: unexpected type for %2%.%3%", ftype, st, f->name);
        }
        fields->append(field);
    }
    // must add padding
    unsigned padding = max_length % 8;
    if (padding != 0) {
        cstring name = structure->refMap->newName("_padding");
        auto field = new Util::JsonArray();
        field->append(name);
        field->append(8 - padding);
        field->append(false);
        fields->append(field);
    }

    unsigned max_length_bytes = (max_length + padding) / 8;
    if (!varbitFound) {
        // ignore
        max_length = 0;
        max_length_bytes = 0;
    }
    ctxt->json->add_header_type(name, fields, max_length_bytes);
}

void PortableCodeGenerator::createTypes(ConversionContext *ctxt,
                                        P4::PortableProgramStructure *structure) {
    for (auto kv : structure->header_types) createStructLike(ctxt, kv.second, structure);
    for (auto kv : structure->metadata_types) createStructLike(ctxt, kv.second, structure);
    for (auto kv : structure->header_union_types) {
        auto st = kv.second;
        auto fields = new Util::JsonArray();
        for (auto f : st->fields) {
            auto field = new Util::JsonArray();
            auto ftype = structure->typeMap->getType(f, true);
            auto ht = ftype->to<IR::Type_Header>();
            CHECK_NULL(ht);
            field->append(f->name.name);
            field->append(ht->name.name);
        }
        ctxt->json->add_union_type(st->name, fields);
    }
    /* TODO */
    // add errors to json
    // add enums to json
}

void PortableCodeGenerator::createScalars(ConversionContext *ctxt,
                                          P4::PortableProgramStructure *structure) {
    if (structure->scalars.empty() && structure->scalarMetadataFields.empty()) return;
    // We may have items in scalarMetadataFields but not scalars
    auto name = structure->scalars.empty() ? "scalars"_cs : structure->scalars.begin()->first;
    ctxt->json->add_metadata("scalars_t"_cs, name);
    ctxt->json->add_header_type("scalars_t"_cs);
    unsigned max_length = 0;
    auto add_field = [&](cstring name, const P4::IR::Declaration *decl) {
        auto field = new Util::JsonArray();
        auto ftype = structure->typeMap->getType(decl, true);
        if (auto type = ftype->to<IR::Type_Bits>()) {
            field->append(name);
            max_length += type->size;
            field->append(type->size);
            field->append(type->isSigned);
        } else if (ftype->is<IR::Type_Boolean>()) {
            field->append(name);
            max_length += 1;
            field->append(1);
            field->append(false);
        } else {
            BUG_CHECK(decl, "%1 is not of Type_Bits or Type_Boolean");
        }
        ctxt->json->add_header_field("scalars_t"_cs, field);
    };
    for (auto kv : structure->scalars) {
        LOG5("Adding a scalar field " << kv.second << " to generated json");
        add_field(kv.second->name.name, kv.second);
    }
    // also output the scalar metadata fields
    for (auto kv : structure->scalarMetadataFields) {
        LOG5("Adding a scalar metadata field " << kv.second << " to generated json");
        add_field(kv.second, kv.first);
    }
    // must add padding
    unsigned padding = max_length % 8;
    if (padding != 0) {
        cstring name = structure->refMap->newName("_padding");
        auto field = new Util::JsonArray();
        field->append(name);
        field->append(8 - padding);
        field->append(false);
        ctxt->json->add_header_field("scalars_t"_cs, field);
    }
}

void PortableCodeGenerator::createHeaders(ConversionContext *ctxt,
                                          P4::PortableProgramStructure *structure) {
    for (auto kv : structure->headers) {
        auto type = kv.second->type->to<IR::Type_StructLike>();
        ctxt->json->add_header(type->controlPlaneName(), kv.second->name);
    }
    for (auto kv : structure->metadata) {
        auto type = kv.second->type->to<IR::Type_StructLike>();
        ctxt->json->add_metadata(type->controlPlaneName(), kv.second->name);
    }
    /* TODO */
    // for (auto kv : header_stacks) {
    //     json->add_header_stack(stack_type, stack_name, stack_size, ids);
    // }
    for (auto kv : structure->header_unions) {
        auto header_name = kv.first;
        auto header_type = kv.second->to<IR::Type_StructLike>()->controlPlaneName();
        // We have to add separately a header instance for all
        // headers in the union.  Each instance will be named with
        // a prefix including the union name, e.g., "u.h"
        Util::JsonArray *fields = new Util::JsonArray();
        for (auto uf : kv.second->to<IR::Type_HeaderUnion>()->fields) {
            auto uft = structure->typeMap->getType(uf, true);
            auto h_name = header_name + "." + uf->controlPlaneName();
            auto h_type = uft->to<IR::Type_StructLike>()->controlPlaneName();
            unsigned id = ctxt->json->add_header(h_type, h_name);
            fields->append(id);
        }
        ctxt->json->add_union(header_type, fields, header_name);
    }
}

void PortableCodeGenerator::createExterns() {
    /* TODO */
    // add parse_vsets to json
    // add meter_arrays to json
    // add counter_arrays to json
    // add register_arrays to json
    // add checksums to json
    // add learn_list to json
    // add calculations to json
    // add extern_instances to json
}

void PortableCodeGenerator::createActions(ConversionContext *ctxt,
                                          P4::PortableProgramStructure *structure) {
    auto cvt = new ActionConverter(ctxt, true);
    for (auto it : structure->actions) {
        auto action = it.first;
        action->apply(*cvt);
    }
}

void PortableCodeGenerator::createGlobals() {
    /* TODO */
    // for (auto e : globals) {
    //     convertExternInstances(e->node->to<IR::Declaration>(), e->to<IR::ExternBlock>());
    // }
}

namespace {

/// Converts the extern instances that are declared at the top level of the
/// program, i.e. outside any parser/control/deparser block.
///
/// This deliberately does *not* descend into non-extern blocks: the externs
/// declared inside a parser, control or deparser are already converted while
/// that block is converted (see ControlConverter / ParserConverter /
/// DeparserConverter), so walking into them here would emit duplicate JSON
/// entries. Only the toplevel extern instances are handled.
///
/// Without this pass a program that declares, say,
///
///     Register<bit<32>, bit<4>>(16) pkt_reg;   // global scope
///
/// produces an empty "register_arrays" section while its actions still
/// reference the register, and bmv2 rejects the program at load time with
/// "Invalid reference to object of type 'register' with name ...".
class GlobalExternConverter : public Inspector {
    ConversionContext *ctxt;
    const bool &emitExterns;

 public:
    GlobalExternConverter(ConversionContext *ctxt, const bool &emitExterns)
        : ctxt(ctxt), emitExterns(emitExterns) {
        CHECK_NULL(ctxt);
        setName("GlobalExternConverter");
    }

    bool preorder(const IR::ToplevelBlock *block) override {
        // Toplevel blocks are not in the IR tree, so iterate their constant
        // values explicitly instead of relying on the default traversal.
        for (auto it : block->constantValue) {
            auto eb = it.second->to<IR::ExternBlock>();
            if (eb == nullptr) continue;
            // The array below is discarded; global action profiles do not
            // matter here because they are also synthesized from each table
            // that uses them.
            ctxt->action_profiles = new Util::JsonArray();
            ExternConverter::cvtExternInstance(ctxt, eb->node->to<IR::Declaration>(), eb,
                                               emitExterns);
        }
        return false;  // do not descend; nested externs are converted per block
    }
};

}  // namespace

void PortableCodeGenerator::createGlobals(ConversionContext *ctxt, const bool &emitExterns) {
    (void)ctxt->toplevel->apply(GlobalExternConverter(ctxt, emitExterns));
}

cstring PortableCodeGenerator::convertHashAlgorithm(cstring algo) {
    if (algo == "CRC16") {
        return "crc16"_cs;
    }
    if (algo == "CRC16_CUSTOM") {
        return "crc16_custom"_cs;
    }
    if (algo == "CRC32") {
        return "crc32"_cs;
    }
    if (algo == "CRC32_CUSTOM") {
        return "crc32_custom"_cs;
    }
    if (algo == "IDENTITY") {
        return "identity"_cs;
    }
    // Target default chosen to be crc16
    if (algo == "TARGET_DEFAULT") {
        return "crc16"_cs;
    }

    return nullptr;
}

ExternConverter_Checksum ExternConverter_Checksum::singleton;
ExternConverter_Counter ExternConverter_Counter::singleton;
ExternConverter_DirectCounter ExternConverter_DirectCounter::singleton;
ExternConverter_Meter ExternConverter_Meter::singleton;
ExternConverter_DirectMeter ExternConverter_DirectMeter::singleton;
ExternConverter_Random ExternConverter_Random::singleton;
ExternConverter_ActionProfile ExternConverter_ActionProfile::singleton;
ExternConverter_ActionSelector ExternConverter_ActionSelector::singleton;
ExternConverter_Digest ExternConverter_Digest::singleton;

Util::IJson *ExternConverter_Checksum::convertExternObject(
    UNUSED ConversionContext *ctxt, UNUSED const P4::ExternMethod *em,
    UNUSED const IR::MethodCallExpression *mc, UNUSED const IR::StatOrDecl *s,
    UNUSED const bool &emitExterns) {
    auto primitive = mkPrimitive("Checksum"_cs);
    return primitive;
}

Util::IJson *ExternConverter_Counter::convertExternObject(UNUSED ConversionContext *ctxt,
                                                          UNUSED const P4::ExternMethod *em,
                                                          UNUSED const IR::MethodCallExpression *mc,
                                                          UNUSED const IR::StatOrDecl *s,
                                                          UNUSED const bool &emitExterns) {
    if (mc->arguments->size() != 1) {
        modelError("Expected 1 argument for %1%", mc);
        return nullptr;
    }
    auto primitive = mkPrimitive("_" + em->originalExternType->name + "_" + em->method->name);
    auto parameters = mkParameters(primitive);
    primitive->emplace_non_null("source_info"_cs, s->sourceInfoJsonObj());
    auto ctr = new Util::JsonObject();
    ctr->emplace("type"_cs, "extern");
    ctr->emplace("value"_cs, em->object->controlPlaneName());
    parameters->append(ctr);
    auto index = ctxt->conv->convert(mc->arguments->at(0)->expression);
    parameters->append(index);
    return primitive;
}

Util::IJson *ExternConverter_DirectCounter::convertExternObject(
    UNUSED ConversionContext *ctxt, UNUSED const P4::ExternMethod *em,
    UNUSED const IR::MethodCallExpression *mc, UNUSED const IR::StatOrDecl *s,
    UNUSED const bool &emitExterns) {
    if (mc->arguments->size() != 0) {
        modelError("Expected 0 argument for %1%", mc);
        return nullptr;
    }
    // Do not generate any code for this operation
    return nullptr;
}

Util::IJson *ExternConverter_Meter::convertExternObject(UNUSED ConversionContext *ctxt,
                                                        UNUSED const P4::ExternMethod *em,
                                                        UNUSED const IR::MethodCallExpression *mc,
                                                        UNUSED const IR::StatOrDecl *s,
                                                        UNUSED const bool &emitExterns) {
    // ActionConverter::convertActionBody lowers this value-returning method by
    // prepending the destination, so the arguments arrive as
    //   [dest, index]          for execute(index)
    //   [dest, index, color]   for execute(index, color)
    // and the destination is emitted last, matching the parameter order of the
    // target's execute(index, value) / execute_color(pre_color, index, value).
    //
    // This used to read the list as though it were the original P4 call, so the
    // prepended destination was mistaken for the colour operand. For the
    // one-argument form that accident produced the correct primitive; for the
    // two-argument form it meant the real colour never reached the target and
    // colour-aware metering silently ran colour blind.
    auto n = mc->arguments->size();
    if (n != 2 && n != 3) {
        modelError("Expected execute(index) or execute(index, color) for %1%", mc);
        return nullptr;
    }
    auto dest = ctxt->conv->convert(mc->arguments->at(0)->expression);
    auto index = ctxt->conv->convert(mc->arguments->at(1)->expression);

    // The colour-aware form needs a primitive of its own: the target's
    // Meter.execute() declares no pre-colour parameter, so handing it a fourth
    // operand would fail config load rather than be quietly ignored.
    auto primitive = mkPrimitive(n == 3 ? "_Meter_execute_color"_cs : "_Meter_execute"_cs);
    auto parameters = mkParameters(primitive);
    primitive->emplace_non_null("source_info"_cs, s->sourceInfoJsonObj());
    auto mtr = new Util::JsonObject();
    mtr->emplace("type"_cs, "extern");
    mtr->emplace("value"_cs, em->object->controlPlaneName());
    parameters->append(mtr);
    if (n == 3) {
        auto color = ctxt->conv->convert(mc->arguments->at(2)->expression);
        parameters->append(color);
    }
    parameters->append(index);
    parameters->append(dest);
    return primitive;
}

Util::IJson *ExternConverter_DirectMeter::convertExternObject(
    UNUSED ConversionContext *ctxt, UNUSED const P4::ExternMethod *em,
    UNUSED const IR::MethodCallExpression *mc, UNUSED const IR::StatOrDecl *s,
    UNUSED const bool &emitExterns) {
    // PSA/PNA DirectMeter::execute() returns a value, so action.cpp's
    // value-returning-method rewrite hands us [destination] for execute() or
    // [destination, pre_color] for execute(color). (v1model's direct_meter has
    // its own converter and an out-parameter, so it never reaches here.)
    //
    // bmv2 runs a direct meter automatically on every table hit and writes the
    // resulting colour into the destination recorded below, which the instance
    // converter emits as result_target. No primitive is generated.
    //
    // The pre-colour form cannot be honoured: the target has no hook to inject
    // a colour into the automatic table-hit execution, so it is reported and
    // ignored rather than silently dropped (the colour would otherwise vanish
    // with no diagnostic).
    auto n = mc->arguments->size();
    if (n < 1 || n > 2) {
        modelError("Expected execute() or execute(color) for %1%", mc);
        return nullptr;
    }
    if (n == 2)
        ::P4::warning(ErrorType::WARN_UNUSED,
                      "%1%: direct-meter pre-colour is ignored by this target", mc);
    auto dest = mc->arguments->at(0);
    ctxt->structure->directMeterMap.setDestination(em->object, dest->expression);
    // Do not generate any code for this operation.
    return nullptr;
}

Util::IJson *ExternConverter_Random::convertExternObject(UNUSED ConversionContext *ctxt,
                                                         UNUSED const P4::ExternMethod *em,
                                                         UNUSED const IR::MethodCallExpression *mc,
                                                         UNUSED const IR::StatOrDecl *s,
                                                         UNUSED const bool &emitExterns) {
    if (mc->arguments->size() != 1) {
        modelError("Expected 1 argument for %1%", mc);
        return nullptr;
    }
    auto primitive = mkPrimitive("modify_field_rng_uniform"_cs);
    auto params = mkParameters(primitive);
    primitive->emplace_non_null("source_info"_cs, em->method->sourceInfoJsonObj());
    auto dest = ctxt->conv->convert(mc->arguments->at(0)->expression);
    // Get low/high values from the declaration instance
    auto di = em->object->to<IR::Declaration_Instance>();
    auto lo = ctxt->conv->convert(di->arguments->at(0)->expression);
    auto hi = ctxt->conv->convert(di->arguments->at(1)->expression);
    params->append(dest);
    params->append(lo);
    params->append(hi);
    return primitive;
}

Util::IJson *ExternConverter_Digest::convertExternObject(UNUSED ConversionContext *ctxt,
                                                         UNUSED const P4::ExternMethod *em,
                                                         UNUSED const IR::MethodCallExpression *mc,
                                                         UNUSED const IR::StatOrDecl *s,
                                                         UNUSED const bool &emitExterns) {
    if (mc->arguments->size() != 1) {
        modelError("Expected 1 arguments for %1%", mc);
        return nullptr;
    }
    auto primitive = mkPrimitive("generate_digest"_cs);
    auto parameters = mkParameters(primitive);
    primitive->emplace_non_null("source_info"_cs, em->method->sourceInfoJsonObj());
    cstring listName = "digest"_cs;
    // If we are supplied a type argument that is a named type use
    // that for the list name.
    if (mc->typeArguments->size() == 1) {
        auto typeArg = mc->typeArguments->at(0);
        if (typeArg->is<IR::Type_Name>()) {
            auto origType = ctxt->refMap->getDeclaration(typeArg->to<IR::Type_Name>()->path, true);
            if (!origType->is<IR::Type_Struct>()) {
                modelError("%1%: expected a struct type", origType->getNode());
                return nullptr;
            }
            auto st = origType->to<IR::Type_Struct>();
            listName = st->controlPlaneName();
        }
    }
    int id = ctxt->createFieldList(mc->arguments->at(0)->expression, listName, true);
    auto cst = new IR::Constant(id);
    ctxt->typeMap->setType(cst, IR::Type_Bits::get(32));
    auto jcst = ctxt->conv->convert(cst);
    parameters->append(jcst);
    return primitive;
}

void ExternConverter_Checksum::convertExternInstance(UNUSED ConversionContext *ctxt,
                                                     UNUSED const IR::Declaration *c,
                                                     UNUSED const IR::ExternBlock *eb,
                                                     UNUSED const bool &emitExterns) { /* TODO */ }

void ExternConverter_Counter::convertExternInstance(UNUSED ConversionContext *ctxt,
                                                    UNUSED const IR::Declaration *c,
                                                    UNUSED const IR::ExternBlock *eb,
                                                    UNUSED const bool &emitExterns) {
    auto inst = c->to<IR::Declaration_Instance>();
    cstring name = inst->controlPlaneName();
    auto sz = eb->findParameterValue("n_counters"_cs);
    CHECK_NULL(sz);
    if (!sz->is<IR::Constant>()) {
        modelError("%1%: expected a constant", sz->getNode());
        return;
    }

    // adding counter instance to counter_arrays[]
    auto jctr = new Util::JsonObject();
    jctr->emplace("name"_cs, name);
    jctr->emplace("id"_cs, nextId("counter_arrays"_cs));
    jctr->emplace_non_null("source_info"_cs, eb->sourceInfoJsonObj());
    jctr->emplace("size"_cs, sz->to<IR::Constant>()->value);
    jctr->emplace("is_direct"_cs, false);
    ctxt->json->counters->append(jctr);

    // add counter instance to extern_instances
    auto extern_obj = new Util::JsonObject();
    extern_obj->emplace("name"_cs, name);
    extern_obj->emplace("id"_cs, nextId("extern_instances"_cs));
    extern_obj->emplace("type"_cs, eb->getName());
    extern_obj->emplace("source_info"_cs, inst->sourceInfoJsonObj());
    ctxt->json->externs->append(extern_obj);
    Util::JsonArray *arr = ctxt->json->insert_array_field(extern_obj, "attribute_values"_cs);

    if (eb->getConstructorParameters()->size() != 2) {
        modelError("%1%: expected two parameters", eb);
        return;
    }
    // first argument to create a counter is just a number, convert and dump to json
    // we get a name from param, type and value from the arguments
    auto attr_obj = new Util::JsonObject();
    auto arg1 = sz->to<IR::Constant>();
    auto param1 = eb->getConstructorParameters()->getParameter(0);
    auto bitwidth = ctxt->typeMap->widthBits(arg1->type, sz->getNode(), false);
    cstring repr = BMV2::stringRepr(arg1->value, ROUNDUP(bitwidth, 8));
    attr_obj->emplace("name"_cs, param1->toString());
    attr_obj->emplace("type"_cs, "hexstr");
    attr_obj->emplace("value"_cs, repr);
    arr->append(attr_obj);

    // second argument is the counter type, this is psa metadata, the converter
    // in conversion context will handle that for us
    auto tp = eb->findParameterValue("type"_cs);
    if (!tp || !tp->is<IR::Declaration_ID>()) {
        modelError("%1%: expected a declaration_id", tp ? tp->getNode() : eb->getNode());
        return;
    }
    auto arg2 = tp->to<IR::Declaration_ID>();
    auto param2 = eb->getConstructorParameters()->getParameter(1);
    auto mem = arg2->toString();
    LOG5("In convertParam with p2 " << param2->toString() << " and mem " << mem);
    auto jsn = ctxt->conv->convertParam(param2, mem);
    arr->append(jsn);
}

void ExternConverter_DirectCounter::convertExternInstance(UNUSED ConversionContext *ctxt,
                                                          UNUSED const IR::Declaration *c,
                                                          UNUSED const IR::ExternBlock *eb,
                                                          UNUSED const bool &emitExterns) {
    auto inst = c->to<IR::Declaration_Instance>();
    cstring name = inst->controlPlaneName();
    auto it = ctxt->structure->directCounterMap.find(name);
    if (it == ctxt->structure->directCounterMap.end()) {
        ::P4::warning(ErrorType::WARN_UNUSED, "%1%: Direct counter not used; ignoring", inst);
    } else {
        auto jctr = new Util::JsonObject();
        jctr->emplace("name"_cs, name);
        jctr->emplace("id"_cs, nextId("counter_arrays"_cs));
        jctr->emplace_non_null("source_info"_cs, eb->sourceInfoJsonObj());
        jctr->emplace("is_direct"_cs, true);
        jctr->emplace("binding"_cs, it->second->controlPlaneName());
        ctxt->json->counters->append(jctr);

        // Adding direct counter to EXTERN_INSTANCES

        auto extern_obj = new Util::JsonObject();
        extern_obj->emplace("name"_cs, name);
        extern_obj->emplace("id"_cs, nextId("extern_instances"_cs));
        extern_obj->emplace("type"_cs, eb->getName());
        extern_obj->emplace("source_info"_cs, inst->sourceInfoJsonObj());
        ctxt->json->externs->append(extern_obj);
        Util::JsonArray *arr = ctxt->json->insert_array_field(extern_obj, "attribute_values"_cs);

        // Direct Counter only has a single argument, which is psa metadata
        // converter in conversion context will handle this for us
        auto tp = eb->findParameterValue("type"_cs);
        if (!tp || !tp->is<IR::Declaration_ID>()) {
            modelError("%1%: expected a declaration_id", tp ? tp->getNode() : eb->getNode());
            return;
        }
        if (eb->getConstructorParameters()->size() < 1) {
            modelError("%1%: expected 1 parameter", eb);
            return;
        }
        auto arg = tp->to<IR::Declaration_ID>();
        auto param = eb->getConstructorParameters()->getParameter(0);
        auto mem = arg->toString();
        LOG5("In convertParam with param " << param->toString() << " and mem " << mem);
        auto jsn = ctxt->conv->convertParam(param, mem);
        arr->append(jsn);
    }
}

void ExternConverter_Meter::convertExternInstance(UNUSED ConversionContext *ctxt,
                                                  UNUSED const IR::Declaration *c,
                                                  UNUSED const IR::ExternBlock *eb,
                                                  UNUSED const bool &emitExterns) {
    if (eb->getConstructorParameters()->size() != 2) {
        modelError("%1%: expected two parameters", eb);
        return;
    }

    auto inst = c->to<IR::Declaration_Instance>();
    cstring name = inst->controlPlaneName();

    // adding meter instance into extern_instances
    auto jext_mtr = new Util::JsonObject();
    jext_mtr->emplace("name"_cs, name);
    jext_mtr->emplace("id"_cs, nextId("extern_instances"_cs));
    jext_mtr->emplace("type"_cs, eb->getName());
    jext_mtr->emplace_non_null("source_info"_cs, eb->sourceInfoJsonObj());
    ctxt->json->externs->append(jext_mtr);

    // adding attributes to meter extern_instance
    Util::JsonArray *arr = ctxt->json->insert_array_field(jext_mtr, "attribute_values"_cs);

    // is_direct
    auto is_direct = new Util::JsonObject();
    is_direct->emplace("name"_cs, "is_direct");
    is_direct->emplace("type"_cs, "hexstr");
    is_direct->emplace("value"_cs, 0);
    arr->append(is_direct);

    // meter_array size
    auto sz = eb->findParameterValue("n_meters"_cs);
    CHECK_NULL(sz);
    if (!sz->is<IR::Constant>()) {
        modelError("%1%: expected a constant", sz->getNode());
        return;
    }
    auto attr_name = eb->getConstructorParameters()->getParameter(0);
    auto s = sz->to<IR::Constant>();
    auto bitwidth = ctxt->typeMap->widthBits(s->type, sz->getNode(), false);
    cstring val = BMV2::stringRepr(s->value, ROUNDUP(bitwidth, 8));
    auto msz = new Util::JsonObject();
    msz->emplace("name"_cs, attr_name->toString());
    msz->emplace("type"_cs, "hexstr");
    msz->emplace("value"_cs, val);
    arr->append(msz);

    // rate count
    auto rc = new Util::JsonObject();
    rc->emplace("name"_cs, "rate_count");
    rc->emplace("type"_cs, "hexstr");
    rc->emplace("value"_cs, 2);
    arr->append(rc);

    // meter kind
    auto mkind = eb->findParameterValue("type"_cs);
    CHECK_NULL(mkind);
    if (!mkind->is<IR::Declaration_ID>()) {
        modelError("%1%: expected a member", mkind->getNode());
        return;
    }
    cstring mkind_name = mkind->to<IR::Declaration_ID>()->name;
    cstring type;
    if (mkind_name == "PACKETS")
        type = "packets"_cs;
    else if (mkind_name == "BYTES")
        type = "bytes"_cs;
    else
        ::P4::error(ErrorType::ERR_UNEXPECTED, "%1%: unexpected meter type", mkind->getNode());
    auto k = new Util::JsonObject();
    k->emplace("name"_cs, "type");
    k->emplace("type"_cs, "string");
    k->emplace("value"_cs, type);
    arr->append(k);
}

void ExternConverter_DirectMeter::convertExternInstance(UNUSED ConversionContext *ctxt,
                                                        UNUSED const IR::Declaration *c,
                                                        UNUSED const IR::ExternBlock *eb,
                                                        UNUSED const bool &emitExterns) {
    auto inst = c->to<IR::Declaration_Instance>();
    cstring name = inst->controlPlaneName();
    auto info = ctxt->structure->directMeterMap.getInfo(c);
    CHECK_NULL(info);
    CHECK_NULL(info->table);
    CHECK_NULL(info->destinationField);

    auto jmtr = new Util::JsonObject();
    jmtr->emplace("name"_cs, name);
    jmtr->emplace("id"_cs, nextId("meter_arrays"_cs));
    jmtr->emplace_non_null("source_info"_cs, eb->sourceInfoJsonObj());
    jmtr->emplace("is_direct"_cs, true);
    jmtr->emplace("rate_count"_cs, 2);
    auto mkind = eb->findParameterValue("type"_cs);
    CHECK_NULL(mkind);
    if (!mkind->is<IR::Declaration_ID>()) {
        modelError("%1%: expected a member", mkind->getNode());
        return;
    }
    cstring mkind_name = mkind->to<IR::Declaration_ID>()->name;
    cstring type;
    if (mkind_name == "PACKETS") {
        type = "packets"_cs;
    } else if (mkind_name == "BYTES") {
        type = "bytes"_cs;
    } else {
        modelError("%1%: unexpected meter type", mkind->getNode());
        return;
    }
    jmtr->emplace("type"_cs, type);
    jmtr->emplace("size"_cs, info->tableSize);
    cstring tblname = info->table->controlPlaneName();
    jmtr->emplace("binding"_cs, tblname);
    auto result = ctxt->conv->convert(info->destinationField);
    jmtr->emplace("result_target"_cs, result->to<Util::JsonObject>()->get("value"_cs));
    ctxt->json->meter_arrays->append(jmtr);
}

void ExternConverter_Random::convertExternInstance(UNUSED ConversionContext *ctxt,
                                                   UNUSED const IR::Declaration *c,
                                                   UNUSED const IR::ExternBlock *eb,
                                                   UNUSED const bool &emitExterns) { /* TODO */ }

void ExternConverter_ActionProfile::convertExternInstance(UNUSED ConversionContext *ctxt,
                                                          UNUSED const IR::Declaration *c,
                                                          UNUSED const IR::ExternBlock *eb,
                                                          UNUSED const bool &emitExterns) {
    auto inst = c->to<IR::Declaration_Instance>();
    cstring name = inst->controlPlaneName();
    // Might call this multiple times if the selector/profile is used more than
    // once in a pipeline, so only add it to the action_profiles once
    if (BMV2::JsonObjects::find_object_by_name(ctxt->action_profiles, name)) return;
    auto action_profile = new Util::JsonObject();
    action_profile->emplace("name"_cs, name);
    action_profile->emplace("id"_cs, nextId("action_profiles"_cs));
    action_profile->emplace_non_null("source_info"_cs, eb->sourceInfoJsonObj());

    auto sz = eb->findParameterValue("size"_cs);
    BUG_CHECK(sz, "%1%Invalid declaration of extern ActionProfile ctor: no size param",
              eb->constructor->srcInfo);

    if (!sz->is<IR::Constant>()) {
        ::P4::error(ErrorType::ERR_EXPECTED, "%1%: expected a constant", sz);
    }
    action_profile->emplace("max_size"_cs, sz->to<IR::Constant>()->value);

    ctxt->action_profiles->append(action_profile);
}

void ExternConverter_ActionSelector::convertExternInstance(UNUSED ConversionContext *ctxt,
                                                           UNUSED const IR::Declaration *c,
                                                           UNUSED const IR::ExternBlock *eb,
                                                           UNUSED const bool &emitExterns) {
    auto inst = c->to<IR::Declaration_Instance>();
    cstring name = inst->controlPlaneName();
    // Might call this multiple times if the selector/profile is used more than
    // once in a pipeline, so only add it to the action_profiles once
    if (BMV2::JsonObjects::find_object_by_name(ctxt->action_profiles, name)) return;
    auto action_profile = new Util::JsonObject();
    action_profile->emplace("name"_cs, name);
    action_profile->emplace("id"_cs, nextId("action_profiles"_cs));
    action_profile->emplace_non_null("source_info"_cs, eb->sourceInfoJsonObj());

    auto sz = eb->findParameterValue("size"_cs);
    BUG_CHECK(sz, "%1%Invalid declaration of extern ActionSelector: no size param",
              eb->constructor->srcInfo);
    if (!sz->is<IR::Constant>()) {
        ::P4::error(ErrorType::ERR_EXPECTED, "%1%: expected a constant", sz);
        return;
    }
    action_profile->emplace("max_size"_cs, sz->to<IR::Constant>()->value);

    auto selector = new Util::JsonObject();
    auto hash = eb->findParameterValue("algo"_cs);

    if (!hash->is<IR::Declaration_ID>()) {
        modelError("%1%: expected a member", hash->getNode());
        return;
    }
    // ExternConverter::convertHashAlgorithm() expects v1model names
    // Convert from PSA/PNA names to v1model names
    PortableCodeGenerator pcg;
    auto v1modelHashName = pcg.convertHashAlgorithm(hash->to<IR::Declaration_ID>()->name);
    if (v1modelHashName.isNullOrEmpty()) {
        ::P4::error(ErrorType::ERR_UNSUPPORTED,
                    "%1%: unsupported hash algorithm for action selector", hash);
        return;
    }
    auto algo = ExternConverter::convertHashAlgorithm(v1modelHashName);
    selector->emplace("algo"_cs, algo);
    auto input = ctxt->get_selector_input(c->to<IR::Declaration_Instance>());
    if (input == nullptr) {
        // the selector is never used by any table, we cannot figure out its
        // input and therefore cannot include it in the JSON
        ::P4::warning(ErrorType::WARN_UNUSED,
                      "Action selector '%1%' is never referenced by a table "
                      "and cannot be included in bmv2 JSON",
                      c);
        return;
    }
    auto j_input = mkArrayField(selector, "input"_cs);
    for (auto expr : *input) {
        auto jk = ctxt->conv->convert(expr);
        j_input->append(jk);
    }
    action_profile->emplace("selector"_cs, selector);

    ctxt->action_profiles->append(action_profile);
}

void ExternConverter_Digest::convertExternInstance(UNUSED ConversionContext *ctxt,
                                                   UNUSED const IR::Declaration *c,
                                                   UNUSED const IR::ExternBlock *eb,
                                                   UNUSED const bool &emitExterns) { /* TODO */ }

}  // namespace P4::BMV2
