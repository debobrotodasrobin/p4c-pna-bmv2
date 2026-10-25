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

#include "pnaNic.h"

#include "frontends/common/model.h"
#include "frontends/p4/cloner.h"
#include "frontends/p4/methodInstance.h"

namespace P4::BMV2 {

using namespace P4::literals;

/// Rewrites the PNA SelectByDirection extern function into an ordinary ternary
/// (IR::Mux), which the bmv2 back end already knows how to lower.
///
/// SelectByDirection<T>(is_network_port, from_net_value, from_host_value) is
/// declared @pure and is defined to be exactly
///   is_network_port ? from_net_value : from_host_value
/// (see p4include/bmv2/pna.p4). It is spelled as an extern only so that the
/// front end leaves it visible to the target back end.
///
/// Without this pass the generic value-returning-extern-function lowering in
/// common/action.cpp treats it like is_host_port()/is_net_port() and emits a
/// bmv2 primitive literally named "SelectByDirection". No target implements
/// that primitive, so the program compiles cleanly but the switch rejects the
/// config at load time with "Unknown primitive action: SelectByDirection".
///
/// Lowering to IR::Mux instead reuses existing support:
/// ExpressionConverter::postorder(const IR::Mux *) already emits bmv2's
/// {"op": "?"} runtime expression, which the runtime evaluates via the SKIP
/// opcode machinery (Expression::push_back_ternary_op). Running before
/// RemoveComplexExpressions means a SelectByDirection used as a table key is
/// hoisted into its key temporary exactly like any other complex key
/// expression, and its arguments (e.g. is_net_port(...)) are hoisted first.
class SelectByDirectionToTernary : public Transform {
    P4::ReferenceMap *refMap;
    P4::TypeMap *typeMap;

 public:
    SelectByDirectionToTernary(P4::ReferenceMap *refMap, P4::TypeMap *typeMap)
        : refMap(refMap), typeMap(typeMap) {
        CHECK_NULL(refMap);
        CHECK_NULL(typeMap);
        setName("P4::BMV2::SelectByDirectionToTernary");
    }
    const IR::Node *postorder(IR::MethodCallExpression *mce) override {
        auto mi = P4::MethodInstance::resolve(mce, refMap, typeMap);
        if (mi == nullptr) return mce;
        auto ef = mi->to<P4::ExternFunction>();
        if (ef == nullptr || ef->method->name != "SelectByDirection") return mce;
        auto args = mce->arguments;
        if (args->size() != 3) {
            ::P4::error(ErrorType::ERR_INVALID,
                        "%1%: SelectByDirection expects 3 arguments, got %2%", mce, args->size());
            return mce;
        }
        // arg0 is the direction predicate, arg1 the net-port value, arg2 the
        // host-port value - matching the @pure reference definition, so the
        // ternary is (cond ? net : host).
        auto cond = args->at(0)->expression;
        auto ifNet = args->at(1)->expression;
        auto ifHost = args->at(2)->expression;
        // The call's own type is not always present in the type map at this
        // point (the generic extern-function lowering in common/action.cpp
        // reads it with mustExist=false for the same reason), but the two
        // value operands are always typed and share the result type T.
        auto resultType = typeMap->getType(ifNet, true);
        auto mux = new IR::Mux(mce->srcInfo, cond, ifNet, ifHost);
        typeMap->setType(mux, resultType);
        return mux;
    }
};

/// Builds the value of an inline bmv2 "field_list" parameter.
///
/// bmv2 requires that value to be an array of {"type": "field", "value":
/// [header, field]} objects: P4Objects::process_single_param() iterates it and
/// rejects (or, worse, asserts on) anything that is not a "field". Converting
/// the argument directly is wrong for a header- or struct-typed argument,
/// because that yields the single object {"type": "header", "value": "..."},
/// which makes bmv2 abort at load time with
///   "in Json::Value::operator[](char const*)const: requires objectValue"
/// ConversionContext::addToFieldList() recursively expands struct-likes into
/// their fields, so it handles headers, structs and field-set literals alike.
static Util::JsonArray *mkFieldListValue(ConversionContext *ctxt, const IR::Expression *expr) {
    auto fields = new Util::JsonArray();
    ctxt->addToFieldList(expr, fields);
    return fields;
}

void PnaCodeGenerator::create(ConversionContext *ctxt, P4::PortableProgramStructure *structure) {
    createTypes(ctxt, structure);
    createHeaders(ctxt, structure);
    createScalars(ctxt, structure);
    createExterns();
    createParsers(ctxt, structure);
    createActions(ctxt, structure);
    createControls(ctxt, structure);
    createDeparsers(ctxt, structure);
    // Extern instances declared at global scope (registers, counters, meters,
    // hashes, ...) are only reachable from the toplevel block, so they need
    // this pass; the block converters above only see the externs declared
    // inside the block they walk. emitExterns is true here for consistency
    // with the ControlConverter instantiation in createControls().
    createGlobals(ctxt, /*emitExterns=*/true);
}

void PnaCodeGenerator::createParsers(ConversionContext *ctxt,
                                     P4::PortableProgramStructure *structure) {
    {
        auto cvt = new ParserConverter(ctxt, "main_parser"_cs);
        auto main_control = structure->parsers.at("main_parser"_cs);
        main_control->apply(*cvt);
    }
}

void PnaCodeGenerator::createControls(ConversionContext *ctxt,
                                      P4::PortableProgramStructure *structure) {
    auto cvt = new BMV2::ControlConverter<Standard::Arch::PNA>(ctxt, "main_control"_cs, true);
    auto main_control = structure->pipelines.at("main_control"_cs);
    main_control->apply(*cvt);
}

void PnaCodeGenerator::createDeparsers(ConversionContext *ctxt,
                                       P4::PortableProgramStructure *structure) {
    {
        auto cvt = new DeparserConverter(ctxt, "main_deparser"_cs);
        auto main_control = structure->deparsers.at("main_deparser"_cs);
        main_control->apply(*cvt);
    }
}

void PnaNicBackend::convert(const IR::ToplevelBlock *tlb) {
    CHECK_NULL(tlb);
    PnaProgramStructure structure(refMap, typeMap);

    auto parsePnaArch = new ParsePnaArchitecture(&structure);
    auto main = tlb->getMain();
    if (!main) return;

    if (main->type->name != "PNA_NIC")
        ::P4::warning(ErrorType::WARN_INVALID,
                      "%1%: the main package should be called PNA_NIC"
                      "; are you using the wrong architecture?",
                      main->type->name);

    main->apply(*parsePnaArch);

    auto evaluator = new P4::EvaluatorPass(refMap, typeMap);
    auto program = tlb->getProgram();
    PassManager simplify = {
        /* TODO */
        // new RenameUserMetadata(refMap, userMetaType, userMetaName),
        new P4::ClearTypeMap(typeMap),  // because the user metadata type has changed
        new P4::SynthesizeActions(refMap, typeMap,
                                  new SkipControls(&structure.non_pipeline_controls)),
        new P4::MoveActionsToTables(refMap, typeMap),
        new P4::TypeChecking(refMap, typeMap),
        new P4::SimplifyControlFlow(typeMap, true),
        new LowerExpressions(typeMap),
        new PassRepeated({new P4::ConstantFolding(typeMap), new P4::StrengthReduction(typeMap)}),
        new P4::TypeChecking(refMap, typeMap),
        new SelectByDirectionToTernary(refMap, typeMap),
        new P4::RemoveComplexExpressions(typeMap,
                                         new ProcessControls(&structure.pipeline_controls)),
        new P4::SimplifyControlFlow(typeMap, true),
        new P4::RemoveAllUnusedDeclarations(P4::RemoveUnusedPolicy()),
        // Converts the DAG into a TREE (at least for expressions)
        // This is important later for conversion to JSON.
        new P4::CloneExpressions(),
        new P4::ClearTypeMap(typeMap),
        evaluator,
        [this, evaluator, structure]() { toplevel = evaluator->getToplevelBlock(); },
    };
    auto hook = options.getDebugHook();
    simplify.addDebugHook(hook);
    program->apply(simplify);

    // map IR node to compile-time allocated resource blocks.
    toplevel->apply(*new P4::BuildResourceMap(&structure.resourceMap));

    main = toplevel->getMain();
    if (!main) return;  // no main
    main->apply(*parsePnaArch);
    if (::P4::errorCount() > 0) return;
    program = toplevel->getProgram();

    PassManager toJson = {new P4::DiscoverStructure(&structure),
                          new InspectPnaProgram(refMap, typeMap, &structure),
                          new ConvertPnaToJson(refMap, typeMap, toplevel, json, &structure)};
    for (const auto &pEnum : *enumMap) {
        auto name = pEnum.first->getName();
        for (const auto &pEntry : *pEnum.second) {
            json->add_enum(name, pEntry.first, pEntry.second);
        }
    }
    program->apply(toJson);
    json->add_program_info(cstring(options.file));
    json->add_meta_info();
}

ExternConverter_Hash ExternConverter_Hash::singleton;
ExternConverter_InternetChecksum ExternConverter_InternetChecksum::singleton;
ExternConverter_Register ExternConverter_Register::singleton;
ExternConverter_add_entry ExternConverter_add_entry::singleton;

Util::IJson *ExternConverter_Hash::convertExternObject(UNUSED ConversionContext *ctxt,
                                                       const P4::ExternMethod *em,
                                                       const IR::MethodCallExpression *mc,
                                                       UNUSED const IR::StatOrDecl *s,
                                                       UNUSED const bool &emitExterns) {
    Util::JsonObject *primitive = nullptr;
    if (mc->arguments->size() == 2)
        primitive = mkPrimitive("_" + em->originalExternType->name + "_" + em->method->name);
    else if (mc->arguments->size() == 4)
        primitive = mkPrimitive("_" + em->originalExternType->name + "_" + "get_hash_mod");
    else {
        modelError("Expected 1 or 3 arguments for %1%", mc);
        return nullptr;
    }
    auto parameters = mkParameters(primitive);
    primitive->emplace_non_null("source_info"_cs, s->sourceInfoJsonObj());
    auto hash = new Util::JsonObject();
    hash->emplace("type"_cs, "extern");
    hash->emplace("value"_cs, em->object->controlPlaneName());
    parameters->append(hash);
    if (mc->arguments->size() == 2) {  // get_hash
        auto dst = ctxt->conv->convertLeftValue(mc->arguments->at(0)->expression);
        auto fieldList = new Util::JsonObject();
        fieldList->emplace("type"_cs, "field_list");
        fieldList->emplace("value"_cs, mkFieldListValue(ctxt, mc->arguments->at(1)->expression));
        parameters->append(dst);
        parameters->append(fieldList);
    } else {  // get_hash with base and mod
        auto dst = ctxt->conv->convertLeftValue(mc->arguments->at(0)->expression);
        auto base = ctxt->conv->convert(mc->arguments->at(1)->expression);
        auto fieldList = new Util::JsonObject();
        fieldList->emplace("type"_cs, "field_list");
        fieldList->emplace("value"_cs, mkFieldListValue(ctxt, mc->arguments->at(2)->expression));
        auto max = ctxt->conv->convert(mc->arguments->at(3)->expression);
        parameters->append(dst);
        parameters->append(base);
        parameters->append(fieldList);
        parameters->append(max);
    }
    return primitive;
}

Util::IJson *ExternConverter_InternetChecksum::convertExternObject(
    UNUSED ConversionContext *ctxt, UNUSED const P4::ExternMethod *em,
    UNUSED const IR::MethodCallExpression *mc, UNUSED const IR::StatOrDecl *s,
    UNUSED const bool &emitExterns) {
    Util::JsonObject *primitive = nullptr;
    if (em->method->name == "add" || em->method->name == "subtract" ||
        em->method->name == "get_state" || em->method->name == "set_state") {
        if (mc->arguments->size() != 1) {
            modelError("Expected 1 argument for %1%", mc);
            return nullptr;
        } else
            primitive = mkPrimitive("_" + em->originalExternType->name + "_" + em->method->name);
    } else if (em->method->name == "get") {
        if (mc->arguments->size() == 1)
            primitive = mkPrimitive("_" + em->originalExternType->name + "_" + em->method->name);
        else if (mc->arguments->size() == 2)
            primitive = mkPrimitive("_" + em->originalExternType->name + "_" + "get_verify");
        else {
            modelError("Unexpected number of arguments for %1%", mc);
            return nullptr;
        }
    } else if (em->method->name == "clear") {
        if (mc->arguments->size() != 0) {
            modelError("Expected 0 argument for %1%", mc);
            return nullptr;
        } else
            primitive = mkPrimitive("_" + em->originalExternType->name + "_" + em->method->name);
    }
    auto parameters = mkParameters(primitive);
    primitive->emplace_non_null("source_info"_cs, s->sourceInfoJsonObj());
    auto cksum = new Util::JsonObject();
    cksum->emplace("type"_cs, "extern");
    cksum->emplace("value"_cs, em->object->controlPlaneName());
    parameters->append(cksum);
    if (em->method->name == "add" || em->method->name == "subtract") {
        auto fieldList = new Util::JsonObject();
        fieldList->emplace("type"_cs, "field_list");
        fieldList->emplace("value"_cs, mkFieldListValue(ctxt, mc->arguments->at(0)->expression));
        parameters->append(fieldList);
    } else if (em->method->name != "clear") {
        if (mc->arguments->size() == 2) {  // get_verify
            auto dst = ctxt->conv->convertLeftValue(mc->arguments->at(0)->expression);
            auto equOp = ctxt->conv->convert(mc->arguments->at(1)->expression);
            parameters->append(dst);
            parameters->append(equOp);
        } else if (mc->arguments->size() == 1) {  // get or get_state or set_state
            auto dst = ctxt->conv->convert(mc->arguments->at(0)->expression);
            parameters->append(dst);
        }
    }
    return primitive;
}

Util::IJson *ExternConverter_Register::convertExternObject(
    UNUSED ConversionContext *ctxt, UNUSED const P4::ExternMethod *em,
    UNUSED const IR::MethodCallExpression *mc, UNUSED const IR::StatOrDecl *s,
    UNUSED const bool &emitExterns) {
    if (em->method->name != "write" && em->method->name != "read") {
        modelError("Unsupported register method %1%", mc);
        return nullptr;
    } else if (em->method->name == "write" && mc->arguments->size() != 2) {
        modelError("Expected 2 arguments for %1%", mc);
        return nullptr;
    } else if (em->method->name == "read" && mc->arguments->size() != 2) {
        modelError("p4c-pna internally requires 2 arguments for %1%", mc);
        return nullptr;
    }
    auto reg = new Util::JsonObject();
    reg->emplace("type"_cs, "register_array");
    cstring name = em->object->controlPlaneName();
    reg->emplace("value"_cs, name);
    if (em->method->name == "read") {
        auto primitive = mkPrimitive("register_read"_cs);
        auto parameters = mkParameters(primitive);
        primitive->emplace_non_null("source_info"_cs, s->sourceInfoJsonObj());
        auto dest = ctxt->conv->convert(mc->arguments->at(0)->expression);
        parameters->append(dest);
        parameters->append(reg);
        auto index = ctxt->conv->convert(mc->arguments->at(1)->expression);
        parameters->append(index);
        return primitive;
    } else if (em->method->name == "write") {
        auto primitive = mkPrimitive("register_write"_cs);
        auto parameters = mkParameters(primitive);
        primitive->emplace_non_null("source_info"_cs, s->sourceInfoJsonObj());
        parameters->append(reg);
        auto index = ctxt->conv->convert(mc->arguments->at(0)->expression);
        parameters->append(index);
        auto value = ctxt->conv->convert(mc->arguments->at(1)->expression);
        parameters->append(value);
        return primitive;
    }
    return nullptr;
}

void ExternConverter_Hash::convertExternInstance(ConversionContext *ctxt, const IR::Declaration *c,
                                                 const IR::ExternBlock *eb,
                                                 UNUSED const bool &emitExterns) {
    auto inst = c->to<IR::Declaration_Instance>();
    cstring name = inst->controlPlaneName();
    // auto pnaStructure = static_cast<PnaCodeGenerator *>(ctxt->structure);
    auto pnaStructure = new PnaCodeGenerator();

    // add hash instance
    auto jhash = new Util::JsonObject();
    jhash->emplace("name"_cs, name);
    jhash->emplace("id"_cs, nextId("extern_instances"_cs));
    jhash->emplace("type"_cs, eb->getName());
    jhash->emplace_non_null("source_info"_cs, inst->sourceInfoJsonObj());
    ctxt->json->externs->append(jhash);

    // add attributes
    if (eb->getConstructorParameters()->size() != 1) {
        modelError("%1%: expected one parameter", eb);
        return;
    }

    Util::JsonArray *arr = ctxt->json->insert_array_field(jhash, "attribute_values"_cs);

    auto algo = eb->findParameterValue("algo"_cs);
    CHECK_NULL(algo);
    if (!algo->is<IR::Declaration_ID>()) {
        modelError("%1%: expected a declaration", algo->getNode());
        return;
    }
    cstring algo_name = algo->to<IR::Declaration_ID>()->name;
    algo_name = pnaStructure->convertHashAlgorithm(algo_name);
    auto k = new Util::JsonObject();
    k->emplace("name"_cs, "algo");
    k->emplace("type"_cs, "string");
    k->emplace("value"_cs, algo_name);
    arr->append(k);
}

void ExternConverter_InternetChecksum::convertExternInstance(UNUSED ConversionContext *ctxt,
                                                             UNUSED const IR::Declaration *c,
                                                             UNUSED const IR::ExternBlock *eb,
                                                             UNUSED const bool &emitExterns) {
    auto inst = c->to<IR::Declaration_Instance>();
    cstring name = inst->controlPlaneName();
    auto trim = inst->controlPlaneName().find(".");
    auto block = inst->controlPlaneName().before(trim);
    auto pnaStructure = static_cast<PnaProgramStructure *>(ctxt->structure);
    auto mainParser = pnaStructure->parsers.at("main_parser"_cs)->controlPlaneName();
    auto mainDeparser = pnaStructure->deparsers.at("main_deparser"_cs)->controlPlaneName();
    if (block != mainParser && block != mainDeparser) {
        ::P4::error(ErrorType::ERR_UNSUPPORTED, "%1%: not supported in pipeline on this target",
                    eb);
    }
    // add checksum instance
    auto jcksum = new Util::JsonObject();
    jcksum->emplace("name"_cs, name);
    jcksum->emplace("id"_cs, nextId("extern_instances"_cs));
    jcksum->emplace("type"_cs, eb->getName());
    jcksum->emplace_non_null("source_info"_cs, inst->sourceInfoJsonObj());
    ctxt->json->externs->append(jcksum);
}

void ExternConverter_Register::convertExternInstance(UNUSED ConversionContext *ctxt,
                                                     UNUSED const IR::Declaration *c,
                                                     UNUSED const IR::ExternBlock *eb,
                                                     UNUSED const bool &emitExterns) {
    size_t paramSize = eb->getConstructorParameters()->size();
    if (paramSize == 2) {
        modelError("%1%: Expecting 1 parameter. Initial value not supported", eb->constructor);
    }
    auto inst = c->to<IR::Declaration_Instance>();
    cstring name = inst->controlPlaneName();
    auto jreg = new Util::JsonObject();
    jreg->emplace("name"_cs, name);
    jreg->emplace("id"_cs, nextId("register_arrays"_cs));
    jreg->emplace_non_null("source_info"_cs, eb->sourceInfoJsonObj());
    auto sz = eb->findParameterValue("size"_cs);
    CHECK_NULL(sz);
    if (!sz->is<IR::Constant>()) {
        modelError("%1%: expected a constant", sz->getNode());
        return;
    }
    if (sz->to<IR::Constant>()->value == 0)
        error(ErrorType::ERR_UNSUPPORTED, "%1%: direct registers are not supported", inst);
    jreg->emplace("size"_cs, sz->to<IR::Constant>()->value);
    if (!eb->instanceType->is<IR::Type_SpecializedCanonical>()) {
        modelError("%1%: Expected a generic specialized type", eb->instanceType);
        return;
    }
    auto st = eb->instanceType->to<IR::Type_SpecializedCanonical>();
    if (st->arguments->size() != 2) {
        modelError("%1%: expected 2 type argument", st);
        return;
    }
    auto regType = st->arguments->at(0);
    if (!regType->is<IR::Type_Bits>()) {
        ::P4::error(ErrorType::ERR_UNSUPPORTED,
                    "%1%: registers with types other than bit<> or int<> are not suppoted", eb);
        return;
    }
    unsigned width = regType->width_bits();
    if (width == 0) {
        ::P4::error(ErrorType::ERR_UNKNOWN, "%1%: unknown width", st->arguments->at(0));
        return;
    }
    jreg->emplace("bitwidth"_cs, width);
    ctxt->json->register_arrays->append(jreg);
}

// add_entry(string action_name, T action_params,
//           ExpireTimeProfileId_t expire_time_profile_id)
//
// Emits the "add_entry" primitive. The only reason this needs a dedicated
// converter (rather than the generic ExternConverter function path) is the
// struct argument: bmv2 reads a struct / list primitive argument as a
// "parameters_vector" object, while the generic expression converter emits a
// bare JSON array. See the declaration in pnaNic.h.
//
// The PNA signature returns AddEntryErrorStatus_t. ActionConverter prepends a
// destination as argument 0 when that result is consumed, so one argument more
// than the declared three means the first argument is a write target; it is
// emitted as a writable reference exactly like the generic value-returning
// path. The bmv2 primitive itself takes only the three declared arguments (it
// does not surface the status), so a program that consumes the result is
// rejected at load time with a parameter-count mismatch rather than silently
// misbehaving.
Util::IJson *ExternConverter_add_entry::convertExternFunction(
    ConversionContext *ctxt, const P4::ExternFunction *ef,
    const IR::MethodCallExpression *mc, const IR::StatOrDecl *s,
    UNUSED const bool emitExterns) {
    auto primitive = mkPrimitive("add_entry"_cs);
    auto parameters = mkParameters(primitive);
    primitive->emplace_non_null("source_info"_cs, s->sourceInfoJsonObj());

    const auto &args = mc->arguments;
    const size_t declared = ef->method->getParameters()->size();  // 3
    const bool destIsArg0 = args->size() == declared + 1;
    const size_t base = destIsArg0 ? 1 : 0;
    if (args->size() != declared && !destIsArg0) {
        modelError("%1%: expected 3 arguments for add_entry", mc);
        return primitive;
    }

    size_t i = 0;
    if (destIsArg0) {
        parameters->append(ctxt->conv->convertLeftValue(args->at(i)->expression));
        ++i;
    }

    // action_name: a compile-time string literal, forwarded as a string param.
    parameters->append(ctxt->conv->convert(args->at(i)->expression));
    ++i;

    // action_params: a struct (or list) packed into a parameters_vector. An
    // empty struct yields an empty vector, which is what DASH's add_entry()
    // calls pass.
    auto pv = new Util::JsonObject();
    pv->emplace("type", "parameters_vector");
    auto arr = new Util::JsonArray();
    auto structArg = args->at(i)->expression;
    if (auto se = structArg->to<IR::StructExpression>()) {
        for (auto comp : se->components)
            arr->append(ctxt->conv->convert(comp->expression, false, true, true));
    } else if (auto le = structArg->to<IR::ListExpression>()) {
        for (auto e : le->components)
            arr->append(ctxt->conv->convert(e, false, true, true));
    } else if (!structArg->is<IR::StructExpression>()) {
        // A single scalar bound to the struct parameter: treat as one element.
        arr->append(ctxt->conv->convert(structArg, false, true, true));
    }
    pv->emplace("value"_cs, arr);
    parameters->append(pv);
    ++i;

    // expire_time_profile_id: a scalar.
    parameters->append(ctxt->conv->convert(args->at(i)->expression));

    return primitive;
}

}  // namespace P4::BMV2
