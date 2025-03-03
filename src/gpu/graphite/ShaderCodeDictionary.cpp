/*
 * Copyright 2022 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/gpu/graphite/ShaderCodeDictionary.h"

#include "include/core/SkSamplingOptions.h"
#include "include/core/SkTileMode.h"
#include "include/effects/SkRuntimeEffect.h"
#include "include/gpu/graphite/Context.h"
#include "src/core/SkColorSpacePriv.h"
#include "src/core/SkColorSpaceXformSteps.h"
#include "src/core/SkRuntimeEffectPriv.h"
#include "src/core/SkSLTypeShared.h"
#include "src/gpu/BlendFormula.h"
#include "src/gpu/Swizzle.h"
#include "src/gpu/graphite/Caps.h"
#include "src/gpu/graphite/ContextUtils.h"
#include "src/gpu/graphite/ReadSwizzle.h"
#include "src/gpu/graphite/Renderer.h"
#include "src/gpu/graphite/RuntimeEffectDictionary.h"
#include "src/sksl/SkSLString.h"
#include "src/sksl/SkSLUtil.h"
#include "src/sksl/codegen/SkSLPipelineStageCodeGenerator.h"
#include "src/sksl/ir/SkSLVarDeclarations.h"

#include <new>

using namespace skia_private;
using namespace SkKnownRuntimeEffects;

namespace skgpu::graphite {

static constexpr int kNoChildren = 0;
static constexpr char kRuntimeShaderName[] = "RuntimeEffect";

static_assert(static_cast<int>(BuiltInCodeSnippetID::kLast) < kSkiaBuiltInReservedCnt);

// The toLinearSrgb and fromLinearSrgb RuntimeEffect intrinsics need to be able to map to and
// from the dst color space and linearSRGB. These are the 10 uniforms needed to allow that.
// These boil down to two copies of the kColorSpaceTransformUniforms uniforms. The first set
// for mapping to LinearSRGB and the second set for mapping from LinearSRGB.
static constexpr Uniform kRuntimeEffectColorSpaceTransformUniforms[] = {
        // to LinearSRGB
        { "flags_toLinear",          SkSLType::kInt },
        { "srcKind_toLinear",        SkSLType::kInt },
        { "gamutTransform_toLinear", SkSLType::kHalf3x3 },
        { "dstKind_toLinear",        SkSLType::kInt },
        { "csXformCoeffs_toLinear",  SkSLType::kHalf4x4 },
        // from LinearSRGB
        { "flags_fromLinear",          SkSLType::kInt },
        { "srcKind_fromLinear",        SkSLType::kInt },
        { "gamutTransform_fromLinear", SkSLType::kHalf3x3 },
        { "dstKind_fromLinear",        SkSLType::kInt },
        { "csXformCoeffs_fromLinear",  SkSLType::kHalf4x4 },
};

namespace {

const char* get_known_rte_name(StableKey key) {
    switch (key) {
#define M(type) case StableKey::k##type : return "KnownRuntimeEffect_" #type;
#define M1(type)
#define M2(type, initializer) case StableKey::k##type : return "KnownRuntimeEffect_" #type;
        SK_ALL_STABLEKEYS(M, M1, M2)
#undef M2
#undef M1
#undef M
    }

    SkUNREACHABLE;
}

std::string get_mangled_name(const std::string& baseName, int manglingSuffix) {
    return baseName + "_" + std::to_string(manglingSuffix);
}

std::string get_mangled_uniform_name(const ShaderInfo& shaderInfo,
                                     const Uniform& uniform,
                                     int manglingSuffix) {
    std::string result;

    if (uniform.isPaintColor()) {
        // Due to deduplication there will only ever be one of these
        result = uniform.name();
    } else {
        result = uniform.name() + std::string("_") + std::to_string(manglingSuffix);
    }
    if (shaderInfo.ssboIndex()) {
        result = EmitStorageBufferAccess("fs", shaderInfo.ssboIndex(), result.c_str());
    }
    return result;
}

std::string get_mangled_sampler_name(const TextureAndSampler& tex, int manglingSuffix) {
    return tex.name() + std::string("_") + std::to_string(manglingSuffix);
}

// Returns an expression to invoke this entry.
std::string emit_expression_for_entry(const ShaderInfo& shaderInfo,
                                      const ShaderNode* node,
                                      ShaderSnippet::Args args) {
    return node->entry()->fExpressionGenerator(shaderInfo, node, args);
}

// Emit the glue code needed to invoke a single static helper isolated within its own scope.
// Glue code will assign the resulting color into a variable `half4 outColor%d`, where the %d is
// filled in with 'node->keyIndex()'.
std::string emit_glue_code_for_entry(const ShaderInfo& shaderInfo,
                                     const ShaderNode* node,
                                     const ShaderSnippet::Args& args,
                                     std::string* funcBody) {
    std::string expr = emit_expression_for_entry(shaderInfo, node, args);
    std::string outputVar = get_mangled_name("outColor", node->keyIndex());
    SkSL::String::appendf(funcBody,
                          "// [%d] %s\n"
                          "half4 %s = %s;",
                          node->keyIndex(),
                          node->entry()->fName,
                          outputVar.c_str(),
                          expr.c_str());
    return outputVar;
}

// Walk the node tree and generate all preambles, accumulating into 'preamble'.
void emit_preambles(const ShaderInfo& shaderInfo,
                    SkSpan<const ShaderNode*> nodes,
                    std::string treeLabel,
                    std::string* preamble) {
    for (int i = 0; i < SkTo<int>(nodes.size()); ++i) {
        const ShaderNode* node = nodes[i];
        std::string nodeLabel = std::to_string(i);
        std::string nextLabel = treeLabel.empty() ? nodeLabel
                                                  : (treeLabel + "<-" + nodeLabel);

        if (node->numChildren() > 0) {
            emit_preambles(shaderInfo, node->children(), nextLabel, preamble);
        }

        std::string nodePreamble = node->entry()->fPreambleGenerator(shaderInfo, node);
        if (!nodePreamble.empty()) {
            SkSL::String::appendf(preamble,
                                "// [%d]   %s: %s\n"
                                "%s\n",
                                node->keyIndex(), nextLabel.c_str(), node->entry()->fName,
                                nodePreamble.c_str());
        }
    }
}

constexpr skgpu::BlendInfo make_simple_blendInfo(skgpu::BlendCoeff srcCoeff,
                                                 skgpu::BlendCoeff dstCoeff) {
    return { skgpu::BlendEquation::kAdd,
             srcCoeff,
             dstCoeff,
             SK_PMColor4fTRANSPARENT,
             skgpu::BlendModifiesDst(skgpu::BlendEquation::kAdd, srcCoeff, dstCoeff) };
}

static constexpr int kNumCoeffModes = (int)SkBlendMode::kLastCoeffMode + 1;
static constexpr skgpu::BlendInfo gBlendTable[kNumCoeffModes] = {
        /* clear */      make_simple_blendInfo(skgpu::BlendCoeff::kZero, skgpu::BlendCoeff::kZero),
        /* src */        make_simple_blendInfo(skgpu::BlendCoeff::kOne,  skgpu::BlendCoeff::kZero),
        /* dst */        make_simple_blendInfo(skgpu::BlendCoeff::kZero, skgpu::BlendCoeff::kOne),
        /* src-over */   make_simple_blendInfo(skgpu::BlendCoeff::kOne,  skgpu::BlendCoeff::kISA),
        /* dst-over */   make_simple_blendInfo(skgpu::BlendCoeff::kIDA,  skgpu::BlendCoeff::kOne),
        /* src-in */     make_simple_blendInfo(skgpu::BlendCoeff::kDA,   skgpu::BlendCoeff::kZero),
        /* dst-in */     make_simple_blendInfo(skgpu::BlendCoeff::kZero, skgpu::BlendCoeff::kSA),
        /* src-out */    make_simple_blendInfo(skgpu::BlendCoeff::kIDA,  skgpu::BlendCoeff::kZero),
        /* dst-out */    make_simple_blendInfo(skgpu::BlendCoeff::kZero, skgpu::BlendCoeff::kISA),
        /* src-atop */   make_simple_blendInfo(skgpu::BlendCoeff::kDA,   skgpu::BlendCoeff::kISA),
        /* dst-atop */   make_simple_blendInfo(skgpu::BlendCoeff::kIDA,  skgpu::BlendCoeff::kSA),
        /* xor */        make_simple_blendInfo(skgpu::BlendCoeff::kIDA,  skgpu::BlendCoeff::kISA),
        /* plus */       make_simple_blendInfo(skgpu::BlendCoeff::kOne,  skgpu::BlendCoeff::kOne),
        /* modulate */   make_simple_blendInfo(skgpu::BlendCoeff::kZero, skgpu::BlendCoeff::kSC),
        /* screen */     make_simple_blendInfo(skgpu::BlendCoeff::kOne,  skgpu::BlendCoeff::kISC)
};

} // anonymous namespace

//--------------------------------------------------------------------------------------------------
// ShaderInfo

ShaderInfo::ShaderInfo(UniquePaintParamsID id,
                       const ShaderCodeDictionary* dict,
                       const RuntimeEffectDictionary* rteDict,
                       const char* ssboIndex)
        : fRuntimeEffectDictionary(rteDict)
        , fSsboIndex(ssboIndex)
        , fSnippetRequirementFlags(SnippetRequirementFlags::kNone) {
    PaintParamsKey key = dict->lookup(id);
    SkASSERT(key.isValid()); // invalid keys should have been caught by invalid paint ID earlier

    fRootNodes = key.getRootNodes(dict, &fShaderNodeAlloc);
    // Aggregate snippet requirements across root nodes and look for fixed-function blend IDs in
    // the root to initialize the HW blend info.
    SkDEBUGCODE(bool fixedFuncBlendFound = false;)
    for (const ShaderNode* root : fRootNodes) {
        // TODO: This is brittle as it relies on PaintParams::toKey() putting the final fixed
        // function blend block at the root level. This can be improved with more structure to the
        // key creation.
        if (root->codeSnippetId() < kBuiltInCodeSnippetIDCount &&
            root->codeSnippetId() >= kFixedFunctionBlendModeIDOffset) {
            SkASSERT(root->numChildren() == 0);
            // This should occur at most once
            SkASSERT(!fixedFuncBlendFound);
            SkDEBUGCODE(fixedFuncBlendFound = true;)

            fBlendMode = static_cast<SkBlendMode>(root->codeSnippetId() -
                                                  kFixedFunctionBlendModeIDOffset);
            SkASSERT(static_cast<int>(fBlendMode) >= 0 &&
                     fBlendMode <= SkBlendMode::kLastCoeffMode);
            fBlendInfo = gBlendTable[static_cast<int>(fBlendMode)];
        } else {
            fSnippetRequirementFlags |= root->requiredFlags();
        }
    }
}

void append_color_output(std::string* mainBody,
                         BlendFormula::OutputType outputType,
                         const char* outColor,
                         const char* inColor) {
    switch (outputType) {
        case BlendFormula::kNone_OutputType:
            SkSL::String::appendf(mainBody, "%s = half4(0.0);", outColor);
            break;
        case BlendFormula::kCoverage_OutputType:
            SkSL::String::appendf(mainBody, "%s = outputCoverage;", outColor);
            break;
        case BlendFormula::kModulate_OutputType:
            SkSL::String::appendf(mainBody, "%s = %s * outputCoverage;", outColor, inColor);
            break;
        case BlendFormula::kSAModulate_OutputType:
            SkSL::String::appendf(mainBody, "%s = %s.a * outputCoverage;", outColor, inColor);
            break;
        case BlendFormula::kISAModulate_OutputType:
            SkSL::String::appendf(
                    mainBody, "%s = (1.0 - %s.a) * outputCoverage;", outColor, inColor);
            break;
        case BlendFormula::kISCModulate_OutputType:
            SkSL::String::appendf(
                    mainBody, "%s = (half4(1.0) - %s) * outputCoverage;", outColor, inColor);
            break;
        default:
            SkUNREACHABLE;
            break;
    }
}

// The current, incomplete, model for shader construction is:
//   - Static code snippets (which can have an arbitrary signature) live in the Graphite
//     pre-compiled modules, which are located at `src/sksl/sksl_graphite_frag.sksl` and
//     `src/sksl/sksl_graphite_frag_es2.sksl`.
//   - Glue code is generated in a `main` method which calls these static code snippets.
//     The glue code is responsible for:
//            1) gathering the correct (mangled) uniforms
//            2) passing the uniforms and any other parameters to the helper method
//   - The result of the final code snippet is then copied into "sk_FragColor".
//   Note: each entry's 'fStaticFunctionName' field is expected to match the name of a function
//   in the Graphite pre-compiled module.
std::string ShaderInfo::toSkSL(const Caps* caps,
                               const RenderStep* step,
                               bool useStorageBuffers,
                               int* numTexturesAndSamplersUsed,
                               int* numPaintUniforms,
                               int* renderStepUniformTotalBytes,
                               int* paintUniformsTotalBytes,
                               bool* hasGradientBuffer,
                               Swizzle writeSwizzle) {
    // If we're doing analytic coverage, we must also be doing shading.
    SkASSERT(step->coverage() == Coverage::kNone || step->performsShading());
    const bool hasStepUniforms = step->numUniforms() > 0 && step->coverage() != Coverage::kNone;
    const bool useStepStorageBuffer = useStorageBuffers && hasStepUniforms;
    const bool useShadingStorageBuffer = useStorageBuffers && step->performsShading();
    const bool useGradientStorageBuffer = useStorageBuffers && (fSnippetRequirementFlags
                                                    & SnippetRequirementFlags::kGradientBuffer);

    const bool defineLocalCoordsVarying = this->needsLocalCoords();
    std::string preamble = EmitVaryings(step,
                                        /*direction=*/"in",
                                        /*emitSsboIndicesVarying=*/useShadingStorageBuffer,
                                        defineLocalCoordsVarying);

    // The uniforms are mangled by having their index in 'fEntries' as a suffix (i.e., "_%d")
    // TODO: replace hard-coded bufferIDs with the backend's step and paint uniform-buffer indices.
    // TODO: The use of these indices is Metal-specific. We should replace these functions with
    // API-independent ones.
    const ResourceBindingRequirements& bindingReqs = caps->resourceBindingRequirements();
    if (hasStepUniforms) {
        if (useStepStorageBuffer) {
            preamble += EmitRenderStepStorageBuffer(/*bufferID=*/1, step->uniforms());
        } else {
            preamble += EmitRenderStepUniforms(/*bufferID=*/1,
                                               bindingReqs.fUniformBufferLayout,
                                               step->uniforms(),
                                               renderStepUniformTotalBytes);
        }
    }

    bool wrotePaintColor = false;
    if (useShadingStorageBuffer) {
        preamble += EmitPaintParamsStorageBuffer(/*bufferID=*/2,
                                                 fRootNodes,
                                                 numPaintUniforms,
                                                 &wrotePaintColor);
        SkSL::String::appendf(&preamble, "uint %s;\n", this->ssboIndex());
    } else {
        preamble += EmitPaintParamsUniforms(/*bufferID=*/2,
                                            bindingReqs.fUniformBufferLayout,
                                            fRootNodes,
                                            numPaintUniforms,
                                            paintUniformsTotalBytes,
                                            &wrotePaintColor);
    }

    if (useGradientStorageBuffer) {
        SkASSERT(caps->storageBufferSupport());

        // In metal the vertex and instance buffer occupy slots 3 and 4 so we use slot 5 in that
        // case. In dawn and vulkan that is not the case so we can occupy slot 3, and those two
        // apis also do separate texture/sampler bindings.
        int binding = bindingReqs.fSeparateTextureAndSamplerBinding ? 3 : 5;
        SkSL::String::appendf(&preamble,
                              "layout (binding=%d) readonly buffer FSGradientBuffer {\n"
                              "    float fsGradientBuffer[];\n"
                              "};\n", binding);
        *hasGradientBuffer = true;
    }

    {
        int binding = 0;
        preamble += EmitTexturesAndSamplers(bindingReqs, fRootNodes, &binding);
        if (step->hasTextures()) {
            preamble += step->texturesAndSamplersSkSL(bindingReqs, &binding);
        }

        // Report back to the caller how many textures and samplers are used.
        if (numTexturesAndSamplersUsed) {
            *numTexturesAndSamplersUsed = binding;
        }
    }

    if (step->emitsPrimitiveColor()) {
        // TODO: Define this in the main body, and then pass it down into snippets like we do with
        // the local coordinates varying.
        preamble += "half4 primitiveColor;";
    }

    // Emit preamble declarations and helper functions required for snippets. In the default case
    // this adds functions that bind a node's specific mangled uniforms to the snippet's
    // implementation in the SkSL modules.
    emit_preambles(*this, fRootNodes, /*treeLabel=*/"", &preamble);

    std::string mainBody = "void main() {";
    // Set initial color. This will typically be optimized out by SkSL in favor of the paint
    // specifying a color with a solid color shader.
    mainBody += "half4 initialColor = half4(0);";

    if (useShadingStorageBuffer) {
        SkSL::String::appendf(&mainBody,
                              "%s = %s.y;\n",
                              this->ssboIndex(),
                              RenderStep::ssboIndicesVarying());
    }

    if (step->emitsPrimitiveColor()) {
        mainBody += step->fragmentColorSkSL();
    }

    // While looping through root nodes to emit shader code, skip the clip shader node if it's found
    // and keep it to apply later during coverage calculation.
    const ShaderNode* clipShaderNode = nullptr;

    // Emit shader main body code, invoking each root node's expression, forwarding the previous
    // node's output to the next.
    static constexpr char kUnusedDstColor[] = "half4(1)";
    static constexpr char kUnusedLocalCoords[] = "float2(0)";
    ShaderSnippet::Args args = {"initialColor",
                                kUnusedDstColor,
                                this->needsLocalCoords() ? "localCoordsVar" : kUnusedLocalCoords};
    for (const ShaderNode* node : fRootNodes) {
        if (node->codeSnippetId() == (int) BuiltInCodeSnippetID::kClipShader) {
            SkASSERT(!clipShaderNode);
            clipShaderNode = node;
            continue;
        }
        // This exclusion of the final Blend can be removed once we've resolved the final
        // blend parenting issue w/in the key
        if (node->codeSnippetId() >= kBuiltInCodeSnippetIDCount ||
            node->codeSnippetId() < kFixedFunctionBlendModeIDOffset) {
            args.fPriorStageOutput = emit_glue_code_for_entry(*this, node, args, &mainBody);
        }
    }

    if (writeSwizzle != Swizzle::RGBA()) {
        SkSL::String::appendf(&mainBody, "%s = %s.%s;", args.fPriorStageOutput.c_str(),
                                                        args.fPriorStageOutput.c_str(),
                                                        writeSwizzle.asString().c_str());
    }

    const char* outColor = args.fPriorStageOutput.c_str();
    const Coverage coverage = step->coverage();
    if (coverage != Coverage::kNone || clipShaderNode) {
        if (useStepStorageBuffer) {
            SkSL::String::appendf(&mainBody,
                                  "uint stepSsboIndex = %s.x;\n",
                                  RenderStep::ssboIndicesVarying());
            mainBody += EmitUniformsFromStorageBuffer("step", "stepSsboIndex", step->uniforms());
        }

        mainBody += "half4 outputCoverage = half4(1);";
        mainBody += step->fragmentCoverageSkSL();

        if (clipShaderNode) {
            std::string clipShaderOutput =
                    emit_glue_code_for_entry(*this, clipShaderNode, args, &mainBody);
            SkSL::String::appendf(&mainBody, "outputCoverage *= %s.a;", clipShaderOutput.c_str());
        }

        // TODO: Determine whether draw is opaque and pass that to GetBlendFormula.
        BlendFormula coverageBlendFormula =
                coverage == Coverage::kLCD
                        ? skgpu::GetLCDBlendFormula(fBlendMode)
                        : skgpu::GetBlendFormula(
                                  /*isOpaque=*/false, /*hasCoverage=*/true, fBlendMode);

        if (this->needsSurfaceColor()) {
            // If this draw uses a non-coherent dst read, we want to keep the existing dst color (or
            // whatever has been previously drawn) when there's no coverage. This helps for batching
            // text draws that need to read from a dst copy for blends. However, this only helps the
            // case where the outer bounding boxes of each letter overlap and not two actual parts
            // of the text.
            DstReadRequirement dstReadReq = caps->getDstReadRequirement();
            if (dstReadReq == DstReadRequirement::kTextureCopy ||
                dstReadReq == DstReadRequirement::kTextureSample) {
                // We don't think any shaders actually output negative coverage, but just as a
                // safety check for floating point precision errors, we compare with <= here. We
                // just check the RGB values of the coverage, since the alpha may not have been set
                // when using LCD. If we are using single-channel coverage, alpha will be equal to
                // RGB anyway.
                mainBody +=
                    "if (all(lessThanEqual(outputCoverage.rgb, half3(0)))) {"
                        "discard;"
                    "}";
            }

            // Use originally-specified BlendInfo and blend with dst manually.
            SkSL::String::appendf(
                    &mainBody,
                    "sk_FragColor = %s * outputCoverage + surfaceColor * (1.0 - outputCoverage);",
                    outColor);
            if (coverage == Coverage::kLCD) {
                SkSL::String::appendf(
                        &mainBody,
                        "half3 lerpRGB = mix(surfaceColor.aaa, %s.aaa, outputCoverage.rgb);"
                        "sk_FragColor.a = max(max(lerpRGB.r, lerpRGB.g), lerpRGB.b);",
                        outColor);
            }

        } else {
            fBlendInfo = {coverageBlendFormula.equation(),
                          coverageBlendFormula.srcCoeff(),
                          coverageBlendFormula.dstCoeff(),
                          SK_PMColor4fTRANSPARENT,
                          coverageBlendFormula.modifiesDst()};

            if (coverage == Coverage::kLCD) {
                mainBody += "outputCoverage.a = max(max(outputCoverage.r, "
                                                       "outputCoverage.g), "
                                                   "outputCoverage.b);";
            }
            append_color_output(
                    &mainBody, coverageBlendFormula.primaryOutput(), "sk_FragColor", outColor);
            if (coverageBlendFormula.hasSecondaryOutput()) {
                append_color_output(&mainBody,
                                    coverageBlendFormula.secondaryOutput(),
                                    "sk_SecondaryFragColor",
                                    outColor);
            }
        }

    } else {
        SkSL::String::appendf(&mainBody, "sk_FragColor = %s;", outColor);
    }
    mainBody += "}\n";

    return preamble + "\n" + mainBody;
}

//--------------------------------------------------------------------------------------------------
// ShaderCodeDictionary

UniquePaintParamsID ShaderCodeDictionary::findOrCreate(PaintParamsKeyBuilder* builder) {
    AutoLockBuilderAsKey keyView{builder};
    if (!keyView->isValid()) {
        return UniquePaintParamsID::InvalidID();
    }

    SkAutoSpinlock lock{fSpinLock};

    UniquePaintParamsID* existingEntry = fPaintKeyToID.find(*keyView);
    if (existingEntry) {
        SkASSERT(fIDToPaintKey[(*existingEntry).asUInt()] == *keyView);
        return *existingEntry;
    }

    // Detach from the builder and copy into the arena
    PaintParamsKey key = keyView->clone(&fArena);
    UniquePaintParamsID newID{SkTo<uint32_t>(fIDToPaintKey.size())};

    fPaintKeyToID.set(key, newID);
    fIDToPaintKey.push_back(key);
    return newID;
}

PaintParamsKey ShaderCodeDictionary::lookup(UniquePaintParamsID codeID) const {
    if (!codeID.isValid()) {
        return PaintParamsKey::Invalid();
    }

    SkAutoSpinlock lock{fSpinLock};
    SkASSERT(codeID.asUInt() < SkTo<uint32_t>(fIDToPaintKey.size()));
    return fIDToPaintKey[codeID.asUInt()];
}

SkSpan<const Uniform> ShaderCodeDictionary::getUniforms(BuiltInCodeSnippetID id) const {
    return fBuiltInCodeSnippets[(int) id].fUniforms;
}

const ShaderSnippet* ShaderCodeDictionary::getEntry(int codeSnippetID) const {
    if (codeSnippetID < 0) {
        return nullptr;
    }

    if (codeSnippetID < kBuiltInCodeSnippetIDCount) {
        return &fBuiltInCodeSnippets[codeSnippetID];
    }

    SkAutoSpinlock lock{fSpinLock};

    if (codeSnippetID >= kSkiaKnownRuntimeEffectsStart &&
        codeSnippetID < kSkiaKnownRuntimeEffectsStart + kStableKeyCnt) {
        int knownRTECodeSnippetID = codeSnippetID - kSkiaKnownRuntimeEffectsStart;

        // TODO(b/238759147): if the snippet hasn't been initialized, get the SkRuntimeEffect and
        // initialize it here
        SkASSERT(fKnownRuntimeEffectCodeSnippets[knownRTECodeSnippetID].fPreambleGenerator);
        return &fKnownRuntimeEffectCodeSnippets[knownRTECodeSnippetID];
    }

    // TODO(b/238759147): handle Android and chrome known runtime effects

    if (codeSnippetID >= kUnknownRuntimeEffectIDStart) {
        int userDefinedCodeSnippetID = codeSnippetID - kUnknownRuntimeEffectIDStart;
        if (userDefinedCodeSnippetID < SkTo<int>(fUserDefinedCodeSnippets.size())) {
            return fUserDefinedCodeSnippets[userDefinedCodeSnippetID].get();
        }
    }

    return nullptr;
}

//--------------------------------------------------------------------------------------------------
namespace {

std::string append_default_snippet_arguments(const ShaderInfo& shaderInfo,
                                             const ShaderNode* node,
                                             const ShaderSnippet::Args& args,
                                             SkSpan<const std::string> childOutputs) {
    std::string code = "(";

    const char* separator = "";

    const ShaderSnippet* entry = node->entry();

    // Append prior-stage output color.
    if (entry->needsPriorStageOutput()) {
        code += args.fPriorStageOutput;
        separator = ", ";
    }

    // Append blender destination color.
    if (entry->needsBlenderDstColor()) {
        code += separator;
        code += args.fBlenderDstColor;
        separator = ", ";
    }

    // Append fragment coordinates.
    if (entry->needsLocalCoords()) {
        code += separator;
        code += args.fFragCoord;
        separator = ", ";
    }

    // Append uniform names.
    for (size_t i = 0; i < entry->fUniforms.size(); ++i) {
        code += separator;
        separator = ", ";
        code += get_mangled_uniform_name(shaderInfo, entry->fUniforms[i], node->keyIndex());
    }

    // Append samplers.
    for (size_t i = 0; i < entry->fTexturesAndSamplers.size(); ++i) {
        code += separator;
        code += get_mangled_sampler_name(entry->fTexturesAndSamplers[i], node->keyIndex());
        separator = ", ";
    }

    // Append child output names.
    for (const std::string& childOutputVar : childOutputs) {
        code += separator;
        separator = ", ";
        code += childOutputVar;
    }
    code.push_back(')');

    return code;
}

std::string emit_helper_function(const ShaderInfo& shaderInfo,
                                 const ShaderNode* node) {
    // Create a helper function that invokes each of the children, then calls the entry's snippet
    // and passes all the child outputs along as arguments.
    const ShaderSnippet* entry = node->entry();
    std::string helperFnName = get_mangled_name(entry->fStaticFunctionName, node->keyIndex());
    std::string helperFn = SkSL::String::printf(
            "half4 %s(half4 inColor, half4 destColor, float2 pos) {",
            helperFnName.c_str());
    TArray<std::string> childOutputVarNames;
    const ShaderSnippet::Args args = {"inColor", "destColor", "pos"};
    for (const ShaderNode* child : node->children()) {
        // Emit glue code into our helper function body (i.e. lifting the child execution up front
        // so their outputs can be passed to the static module function for the node's snippet).
        childOutputVarNames.push_back(emit_glue_code_for_entry(shaderInfo, child, args, &helperFn));
    }

    // Finally, invoke the snippet from the helper function, passing uniforms and child outputs.
    std::string snippetArgList = append_default_snippet_arguments(shaderInfo, node,
                                                                  args, childOutputVarNames);
    SkSL::String::appendf(&helperFn,
                              "return %s%s;"
                          "}",
                          entry->fStaticFunctionName, snippetArgList.c_str());
    return helperFn;
}

// If we have no children, the default expression just calls a built-in snippet with the signature:
//     half4 BuiltinFunctionName(/* default snippet arguments */);
//
// If we do have children, we will have created a glue function in the preamble and that is called
// instead. Its signature looks like this:
//     half4 BuiltinFunctionName_N(half4 inColor, half4 destColor, float2 pos);

std::string GenerateDefaultExpression(const ShaderInfo& shaderInfo,
                                      const ShaderNode* node,
                                      const ShaderSnippet::Args& args) {
    if (node->numChildren() == 0) {
        // We don't have any children; return an expression which invokes the snippet directly.
        return node->entry()->fStaticFunctionName +
               append_default_snippet_arguments(shaderInfo, node, args, /*childOutputs=*/{});
    } else {
        // Return an expression which invokes the helper function from the preamble.
        std::string helperFnName =
                get_mangled_name(node->entry()->fStaticFunctionName, node->keyIndex());
        return SkSL::String::printf(
                "%s(%.*s, %.*s, %.*s)",
                helperFnName.c_str(),
                (int)args.fPriorStageOutput.size(), args.fPriorStageOutput.data(),
                (int)args.fBlenderDstColor.size(),  args.fBlenderDstColor.data(),
                (int)args.fFragCoord.size(),        args.fFragCoord.data());
    }
}

// If we have no children, we don't need to add anything into the preamble.
// If we have child entries, we create a function in the preamble with a signature of:
//     half4 BuiltinFunctionName_N(half4 inColor, half4 destColor, float2 pos) { ... }
// This function invokes each child in sequence, and then calls the built-in function, passing all
// uniforms and child outputs along:
//     half4 BuiltinFunctionName(/* all uniforms as parameters */,
//                               /* all child output variable names as parameters */);
std::string GenerateDefaultPreamble(const ShaderInfo& shaderInfo,
                                    const ShaderNode* node) {
    if (node->numChildren() > 0) {
        // Create a helper function which invokes all the child snippets.
        return emit_helper_function(shaderInfo, node);
    } else {
        // We don't need a helper function
        return "";
    }
}

//--------------------------------------------------------------------------------------------------
static constexpr Uniform kDstReadSampleUniforms[] = {
        { "dstTextureCoords", SkSLType::kFloat4 },
};

static constexpr TextureAndSampler kDstReadSampleTexturesAndSamplers[] = {
        {"dstSampler"},
};

std::string GenerateDstReadSampleExpression(const ShaderInfo& shaderInfo,
                                            const ShaderNode* node,
                                            const ShaderSnippet::Args& args) {
    const ShaderSnippet* entry = node->entry();
    std::string sampler =
            get_mangled_sampler_name(entry->fTexturesAndSamplers[0], node->keyIndex());
    std::string coords =
            get_mangled_uniform_name(shaderInfo, entry->fUniforms[0], node->keyIndex());
    std::string helperFnName = get_mangled_name(entry->fStaticFunctionName, node->keyIndex());

    return SkSL::String::printf("%s(%s, %s)",
                                helperFnName.c_str(),
                                coords.c_str(),
                                sampler.c_str());
}

std::string GenerateDstReadSamplePreamble(const ShaderInfo& shaderInfo, const ShaderNode* node) {
    std::string helperFnName =
            get_mangled_name(node->entry()->fStaticFunctionName, node->keyIndex());

    return SkSL::String::printf(
            "half4 surfaceColor;"  // we save off the original dstRead color to combine w/ coverage
            "half4 %s(float4 coords, sampler2D dstSampler) {"
                "surfaceColor = sample(dstSampler, (sk_FragCoord.xy - coords.xy) * coords.zw);"
                "return surfaceColor;"
            "}",
            helperFnName.c_str());
}

//--------------------------------------------------------------------------------------------------
std::string GenerateDstReadFetchExpression(const ShaderInfo& shaderInfo,
                                           const ShaderNode* node,
                                           const ShaderSnippet::Args& args) {
    std::string helperFnName =
            get_mangled_name(node->entry()->fStaticFunctionName, node->keyIndex());

    return SkSL::String::printf("%s()", helperFnName.c_str());
}

std::string GenerateDstReadFetchPreamble(const ShaderInfo& shaderInfo, const ShaderNode* node) {
    std::string helperFnName =
            get_mangled_name(node->entry()->fStaticFunctionName, node->keyIndex());

    return SkSL::String::printf(
            "half4 surfaceColor;"  // we save off the original dstRead color to combine w/ coverage
            "half4 %s() {"
                "surfaceColor = sk_LastFragColor;"
                "return surfaceColor;"
            "}",
            helperFnName.c_str());
}

//--------------------------------------------------------------------------------------------------
static constexpr int kNumClipShaderChildren = 1;

std::string GenerateClipShaderExpression(const ShaderInfo& shaderInfo,
                                         const ShaderNode* node,
                                         const ShaderSnippet::Args& args) {
    SkASSERT(node->numChildren() == kNumClipShaderChildren);
    static constexpr char kUnusedSrcColor[] = "half4(1)";
    static constexpr char kUnusedDstColor[] = "half4(1)";
    return emit_expression_for_entry(
            shaderInfo, node->child(0), {kUnusedSrcColor, kUnusedDstColor, "sk_FragCoord.xy"});
}

std::string GenerateClipShaderPreamble(const ShaderInfo& shaderInfo, const ShaderNode* node) {
    // No preamble is used for clip shaders. The child shader is called directly with sk_FragCoord.
    return "";
}

//--------------------------------------------------------------------------------------------------
static constexpr int kFourStopGradient = 4;
static constexpr int kEightStopGradient = 8;

static constexpr Uniform kLinearGradientUniforms4[] = {
        { "colors",      SkSLType::kFloat4, kFourStopGradient },
        { "offsets",     SkSLType::kFloat4 },
        { "tilemode",    SkSLType::kInt },
        { "colorSpace",  SkSLType::kInt },
        { "doUnPremul",  SkSLType::kInt },
};
static constexpr Uniform kLinearGradientUniforms8[] = {
        { "colors",      SkSLType::kFloat4, kEightStopGradient },
        { "offsets",     SkSLType::kFloat4, 2 },
        { "tilemode",    SkSLType::kInt },
        { "colorSpace",  SkSLType::kInt },
        { "doUnPremul",  SkSLType::kInt },
};
static constexpr Uniform kLinearGradientUniformsTexture[] = {
        { "numStops",    SkSLType::kInt },
        { "tilemode",    SkSLType::kInt },
        { "colorSpace",  SkSLType::kInt },
        { "doUnPremul",  SkSLType::kInt },
};

static constexpr Uniform kLinearGradientUniformsBuffer[] = {
        { "numStops",    SkSLType::kInt },
        { "bufferOffset",   SkSLType::kInt },
        { "tilemode",    SkSLType::kInt },
        { "colorSpace",  SkSLType::kInt },
        { "doUnPremul",  SkSLType::kInt },
};

static constexpr Uniform kRadialGradientUniforms4[] = {
        { "colors",      SkSLType::kFloat4, kFourStopGradient },
        { "offsets",     SkSLType::kFloat4 },
        { "tilemode",    SkSLType::kInt },
        { "colorSpace",  SkSLType::kInt },
        { "doUnPremul",  SkSLType::kInt },
};
static constexpr Uniform kRadialGradientUniforms8[] = {
        { "colors",      SkSLType::kFloat4, kEightStopGradient },
        { "offsets",     SkSLType::kFloat4, 2 },
        { "tilemode",    SkSLType::kInt },
        { "colorSpace",  SkSLType::kInt },
        { "doUnPremul",  SkSLType::kInt },
};
static constexpr Uniform kRadialGradientUniformsTexture[] = {
        { "numStops",    SkSLType::kInt },
        { "tilemode",    SkSLType::kInt },
        { "colorSpace",  SkSLType::kInt },
        { "doUnPremul",  SkSLType::kInt },
};
static constexpr Uniform kRadialGradientUniformsBuffer[] = {
        { "numStops",    SkSLType::kInt },
        { "bufferOffset",   SkSLType::kInt },
        { "tilemode",    SkSLType::kInt },
        { "colorSpace",  SkSLType::kInt },
        { "doUnPremul",  SkSLType::kInt },
};

static constexpr Uniform kSweepGradientUniforms4[] = {
        { "colors",      SkSLType::kFloat4, kFourStopGradient },
        { "offsets",     SkSLType::kFloat4 },
        { "bias",        SkSLType::kFloat },
        { "scale",       SkSLType::kFloat },
        { "tilemode",    SkSLType::kInt },
        { "colorSpace",  SkSLType::kInt },
        { "doUnPremul",  SkSLType::kInt },
};
static constexpr Uniform kSweepGradientUniforms8[] = {
        { "colors",      SkSLType::kFloat4, kEightStopGradient },
        { "offsets",     SkSLType::kFloat4, 2 },
        { "bias",        SkSLType::kFloat },
        { "scale",       SkSLType::kFloat },
        { "tilemode",    SkSLType::kInt },
        { "colorSpace",  SkSLType::kInt },
        { "doUnPremul",  SkSLType::kInt },
};
static constexpr Uniform kSweepGradientUniformsTexture[] = {
        { "bias",        SkSLType::kFloat },
        { "scale",       SkSLType::kFloat },
        { "numStops",    SkSLType::kInt },
        { "tilemode",    SkSLType::kInt },
        { "colorSpace",  SkSLType::kInt },
        { "doUnPremul",  SkSLType::kInt },
};
static constexpr Uniform kSweepGradientUniformsBuffer[] = {
        { "bias",        SkSLType::kFloat },
        { "scale",       SkSLType::kFloat },
        { "numStops",    SkSLType::kInt },
        { "bufferOffset",   SkSLType::kInt },
        { "tilemode",    SkSLType::kInt },
        { "colorSpace",  SkSLType::kInt },
        { "doUnPremul",  SkSLType::kInt },
};

static constexpr Uniform kConicalGradientUniforms4[] = {
        { "colors",      SkSLType::kFloat4, kFourStopGradient },
        { "offsets",     SkSLType::kFloat4 },
        { "radius0",     SkSLType::kFloat },
        { "dRadius",     SkSLType::kFloat },
        { "a",           SkSLType::kFloat },
        { "invA",        SkSLType::kFloat },
        { "tilemode",    SkSLType::kInt },
        { "colorSpace",  SkSLType::kInt },
        { "doUnPremul",  SkSLType::kInt },
};
static constexpr Uniform kConicalGradientUniforms8[] = {
        { "colors",      SkSLType::kFloat4, kEightStopGradient },
        { "offsets",     SkSLType::kFloat4, 2 },
        { "radius0",     SkSLType::kFloat },
        { "dRadius",     SkSLType::kFloat },
        { "a",           SkSLType::kFloat },
        { "invA",        SkSLType::kFloat },
        { "tilemode",    SkSLType::kInt },
        { "colorSpace",  SkSLType::kInt },
        { "doUnPremul",  SkSLType::kInt },
};
static constexpr Uniform kConicalGradientUniformsTexture[] = {
        { "radius0",     SkSLType::kFloat },
        { "dRadius",     SkSLType::kFloat },
        { "a",           SkSLType::kFloat },
        { "invA",        SkSLType::kFloat },
        { "numStops",    SkSLType::kInt },
        { "tilemode",    SkSLType::kInt },
        { "colorSpace",  SkSLType::kInt },
        { "doUnPremul",  SkSLType::kInt },
};
static constexpr Uniform kConicalGradientUniformsBuffer[] = {
        { "radius0",     SkSLType::kFloat },
        { "dRadius",     SkSLType::kFloat },
        { "a",           SkSLType::kFloat },
        { "invA",        SkSLType::kFloat },
        { "numStops",    SkSLType::kInt },
        { "bufferOffset",   SkSLType::kInt },
        { "tilemode",    SkSLType::kInt },
        { "colorSpace",  SkSLType::kInt },
        { "doUnPremul",  SkSLType::kInt },
};

static constexpr TextureAndSampler kTextureGradientTexturesAndSamplers[] = {
        {"colorAndOffsetSampler"},
};

static constexpr char kLinearGradient4Name[] = "sk_linear_grad_4_shader";
static constexpr char kLinearGradient8Name[] = "sk_linear_grad_8_shader";
static constexpr char kLinearGradientTextureName[] = "sk_linear_grad_tex_shader";
static constexpr char kLinearGradientBufferName[] = "sk_linear_grad_buf_shader";

static constexpr char kRadialGradient4Name[] = "sk_radial_grad_4_shader";
static constexpr char kRadialGradient8Name[] = "sk_radial_grad_8_shader";
static constexpr char kRadialGradientTextureName[] = "sk_radial_grad_tex_shader";
static constexpr char kRadialGradientBufferName[] = "sk_radial_grad_buf_shader";

static constexpr char kSweepGradient4Name[] = "sk_sweep_grad_4_shader";
static constexpr char kSweepGradient8Name[] = "sk_sweep_grad_8_shader";
static constexpr char kSweepGradientTextureName[] = "sk_sweep_grad_tex_shader";
static constexpr char kSweepGradientBufferName[] = "sk_sweep_grad_buf_shader";

static constexpr char kConicalGradient4Name[] = "sk_conical_grad_4_shader";
static constexpr char kConicalGradient8Name[] = "sk_conical_grad_8_shader";
static constexpr char kConicalGradientTextureName[] = "sk_conical_grad_tex_shader";
static constexpr char kConicalGradientBufferName[] = "sk_conical_grad_buf_shader";

// These expression and preamble generators are only needed until we support passing unsized
// arrays into SkSL module functions (b/343510513).
std::string GenerateGradientBufferExpression(const ShaderInfo& shaderInfo,
                                             const ShaderNode* node,
                                             const ShaderSnippet::Args& args) {
    std::string helperFnName =
            get_mangled_name(node->entry()->fStaticFunctionName, node->keyIndex());
    return helperFnName + append_default_snippet_arguments(shaderInfo, node, args, {});
}

std::string GenerateGradientBufferPreamble(const ShaderInfo& shaderInfo,
                                           const ShaderNode* node) {
    SkASSERT(node->codeSnippetId() == (int) BuiltInCodeSnippetID::kLinearGradientShaderBuffer ||
             node->codeSnippetId() == (int) BuiltInCodeSnippetID::kRadialGradientShaderBuffer ||
             node->codeSnippetId() == (int) BuiltInCodeSnippetID::kSweepGradientShaderBuffer ||
             node->codeSnippetId() == (int) BuiltInCodeSnippetID::kConicalGradientShaderBuffer);
    SkASSERT(node->numChildren() == 0);

    const char* gradArgs;
    const char* layoutFnCall;
    switch (node->codeSnippetId()) {
        case (int) BuiltInCodeSnippetID::kLinearGradientShaderBuffer:
            gradArgs = "float2 coords";
            layoutFnCall = "linear_grad_layout(coords)";
            break;
        case (int) BuiltInCodeSnippetID::kRadialGradientShaderBuffer:
            gradArgs = "float2 coords";
            layoutFnCall = "radial_grad_layout(coords)";
            break;
        case (int) BuiltInCodeSnippetID::kSweepGradientShaderBuffer:
            gradArgs = "float2 coords, float biasParam, float scaleParam";
            layoutFnCall = "sweep_grad_layout(biasParam, scaleParam, coords)";
            break;
        case (int) BuiltInCodeSnippetID::kConicalGradientShaderBuffer:
            gradArgs = "float2 coords, float radius0Param, float dRadiusParam, "
                            "float aParam, float invAParam";
            layoutFnCall = "conical_grad_layout(radius0Param, dRadiusParam, "
                                "aParam, invAParam, coords)";
            break;
    }

    std::string helperFnName =
            get_mangled_name(node->entry()->fStaticFunctionName, node->keyIndex());
    return SkSL::String::printf(
                "half4 %s(%s, int numStops, int bufferOffset, int tileMode,"
                         "int colorSpace, int doUnpremul) {"
                    "float2 t = %s;"
                    "t = tile_grad(tileMode, t);"

                    // Colorize
                    "half4 color = half4(0);"
                    "if (t.y >= 0) {"
                        "if (t.x == 0) {"
                            // Start from 1 since bufferOffset + 0 is holding the
                            // color stop's offset value.
                            "color = half4(fsGradientBuffer[bufferOffset + 1],"
                                          "fsGradientBuffer[bufferOffset + 2],"
                                          "fsGradientBuffer[bufferOffset + 3],"
                                          "fsGradientBuffer[bufferOffset + 4]);"
                        "} else if (t.x == 1) {"
                            "int endBufferIdx = bufferOffset + numStops * 5;"
                            "color = half4(fsGradientBuffer[endBufferIdx - 4],"
                                          "fsGradientBuffer[endBufferIdx - 3],"
                                          "fsGradientBuffer[endBufferIdx - 2],"
                                          "fsGradientBuffer[endBufferIdx - 1]);"
                        "} else {"
                            // Binary search for the matching adjacent offsets
                            // running log(numStops).
                            "int low = 0;"
                            "int high = numStops - 1;"
                            "for (int i = 1; i < numStops; i += i) {"
                                "int mid = (low + high) / 2;"
                                "float offset = fsGradientBuffer[bufferOffset + mid * 5];"
                                "if (t.x < offset) {"
                                    "high = mid;"
                                "} else {"
                                    "low = mid;"
                                "}"
                            "}"
                            "int lowBufferIdx = bufferOffset + low * 5;"
                            "float lowOffset = fsGradientBuffer[lowBufferIdx];"
                            "half4 lowColor = half4(fsGradientBuffer[lowBufferIdx + 1],"
                                                   "fsGradientBuffer[lowBufferIdx + 2],"
                                                   "fsGradientBuffer[lowBufferIdx + 3],"
                                                   "fsGradientBuffer[lowBufferIdx + 4]);"

                            "int highBufferIdx = bufferOffset + high * 5;"
                            "float highOffset = fsGradientBuffer[highBufferIdx];"
                            "if (highOffset == lowOffset) {"
                                // If the t value falls exactly on a color stop, both lowOffset
                                // and highOffset will be exactly the same so we avoid having
                                // 0/0=NaN as our mix value.
                                "color = lowColor;"
                            "} else {"
                                "half4 highColor = half4(fsGradientBuffer[highBufferIdx + 1],"
                                                        "fsGradientBuffer[highBufferIdx + 2],"
                                                        "fsGradientBuffer[highBufferIdx + 3],"
                                                        "fsGradientBuffer[highBufferIdx + 4]);"

                                "color = half4(mix(lowColor,"
                                                  "highColor,"
                                                  "(t.x - lowOffset) /"
                                                  "(highOffset - lowOffset)));"
                            "}"
                        "}"
                    "}"

                    "return interpolated_to_rgb_unpremul(color,"
                                                        "colorSpace,"
                                                        "doUnpremul);"
                "}",
                helperFnName.c_str(),
                gradArgs,
                layoutFnCall);
}


//--------------------------------------------------------------------------------------------------
static constexpr Uniform kSolidShaderUniforms[] = {
        { "color", SkSLType::kFloat4 }
};

static constexpr char kSolidShaderName[] = "sk_solid_shader";

//--------------------------------------------------------------------------------------------------
static constexpr Uniform kPaintColorUniforms[] = { Uniform::PaintColor() };

static constexpr char kRGBPaintColorName[] = "sk_rgb_opaque";
static constexpr char kAlphaOnlyPaintColorName[] = "sk_alpha_only";

//--------------------------------------------------------------------------------------------------
static constexpr Uniform kLocalMatrixShaderUniforms[] = {
        { "localMatrix", SkSLType::kFloat4x4 },
};

static constexpr int kNumLocalMatrixShaderChildren = 1;

static constexpr char kLocalMatrixShaderName[] = "LocalMatrix";

// Create a helper function that multiplies coordinates by a local matrix, invokes the child
// entry with those updated coordinates, and returns the result. This helper function meets the
// requirements for use with GenerateDefaultExpression, so there's no need to have a separate
// special GenerateLocalMatrixExpression.
std::string GenerateLocalMatrixPreamble(const ShaderInfo& shaderInfo,
                                        const ShaderNode* node) {
    SkASSERT(node->codeSnippetId() == (int) BuiltInCodeSnippetID::kLocalMatrixShader);
    SkASSERT(node->numChildren() == kNumLocalMatrixShaderChildren);

    // Get the child's evaluation expression.
    static constexpr char kUnusedDestColor[] = "half4(1)";
    std::string childExpr = emit_expression_for_entry(shaderInfo, node->child(0),
                                                      {"inColor", kUnusedDestColor, "coords"});
    std::string localMatrixUni =
            get_mangled_uniform_name(shaderInfo, node->entry()->fUniforms[0], node->keyIndex());

    std::string helperFnName =
            get_mangled_name(node->entry()->fStaticFunctionName, node->keyIndex());
    return SkSL::String::printf("half4 %s(half4 inColor, half4 destColor, float2 coords) {"
                                    "coords = (%s * coords.xy01).xy;"
                                    "return %s;"
                                "}",
                                helperFnName.c_str(),
                                localMatrixUni.c_str(),
                                childExpr.c_str());
}

//--------------------------------------------------------------------------------------------------
static constexpr Uniform kImageShaderUniforms[] = {
        { "invImgSize",            SkSLType::kFloat2 },
        { "subset",                SkSLType::kFloat4 },
        { "tilemodeX",             SkSLType::kInt },
        { "tilemodeY",             SkSLType::kInt },
        { "filterMode",            SkSLType::kInt },
        // The next 5 uniforms are for the color space transformation
        { "csXformFlags",          SkSLType::kInt },
        { "csXformSrcKind",        SkSLType::kInt },
        { "csXformGamutTransform", SkSLType::kHalf3x3 },
        { "csXformDstKind",        SkSLType::kInt },
        { "csXformCoeffs",         SkSLType::kHalf4x4 },
};

static constexpr Uniform kCubicImageShaderUniforms[] = {
        { "invImgSize",            SkSLType::kFloat2 },
        { "subset",                SkSLType::kFloat4 },
        { "tilemodeX",             SkSLType::kInt },
        { "tilemodeY",             SkSLType::kInt },
        { "cubicCoeffs",           SkSLType::kHalf4x4 },
        // The next 5 uniforms are for the color space transformation
        { "csXformFlags",          SkSLType::kInt },
        { "csXformSrcKind",        SkSLType::kInt },
        { "csXformGamutTransform", SkSLType::kHalf3x3 },
        { "csXformDstKind",        SkSLType::kInt },
        { "csXformCoeffs",         SkSLType::kHalf4x4 },
};

static constexpr Uniform kHWImageShaderUniforms[] = {
        { "invImgSize",            SkSLType::kFloat2 },
        // The next 5 uniforms are for the color space transformation
        { "csXformFlags",          SkSLType::kInt },
        { "csXformSrcKind",        SkSLType::kInt },
        { "csXformGamutTransform", SkSLType::kHalf3x3 },
        { "csXformDstKind",        SkSLType::kInt },
        { "csXformCoeffs",         SkSLType::kHalf4x4 },
};

static constexpr TextureAndSampler kISTexturesAndSamplers[] = {
        {"sampler"},
};

static_assert(0 == static_cast<int>(SkTileMode::kClamp),  "ImageShader code depends on SkTileMode");
static_assert(1 == static_cast<int>(SkTileMode::kRepeat), "ImageShader code depends on SkTileMode");
static_assert(2 == static_cast<int>(SkTileMode::kMirror), "ImageShader code depends on SkTileMode");
static_assert(3 == static_cast<int>(SkTileMode::kDecal),  "ImageShader code depends on SkTileMode");

static_assert(0 == static_cast<int>(SkFilterMode::kNearest),
              "ImageShader code depends on SkFilterMode");
static_assert(1 == static_cast<int>(SkFilterMode::kLinear),
              "ImageShader code depends on SkFilterMode");

static_assert(0 == static_cast<int>(ReadSwizzle::kRGBA),
              "ImageShader code depends on ReadSwizzle");
static_assert(1 == static_cast<int>(ReadSwizzle::kRGB1),
              "ImageShader code depends on ReadSwizzle");
static_assert(2 == static_cast<int>(ReadSwizzle::kRRR1),
              "ImageShader code depends on ReadSwizzle");
static_assert(3 == static_cast<int>(ReadSwizzle::kBGRA),
              "ImageShader code depends on ReadSwizzle");
static_assert(4 == static_cast<int>(ReadSwizzle::k000R),
              "ImageShader code depends on ReadSwizzle");

static constexpr char kImageShaderName[] = "sk_image_shader";
static constexpr char kCubicImageShaderName[] = "sk_cubic_image_shader";
static constexpr char kHWImageShaderName[] = "sk_hw_image_shader";

//--------------------------------------------------------------------------------------------------

static constexpr Uniform kYUVImageShaderUniforms[] = {
        { "invImgSizeY",           SkSLType::kFloat2 },
        { "invImgSizeUV",          SkSLType::kFloat2 },  // Relative to Y's texel space
        { "subset",                SkSLType::kFloat4 },
        { "linearFilterUVInset",   SkSLType::kFloat2 },
        { "tilemodeX",             SkSLType::kInt },
        { "tilemodeY",             SkSLType::kInt },
        { "filterModeY",           SkSLType::kInt },
        { "filterModeUV",          SkSLType::kInt },
        { "channelSelectY",        SkSLType::kHalf4 },
        { "channelSelectU",        SkSLType::kHalf4 },
        { "channelSelectV",        SkSLType::kHalf4 },
        { "channelSelectA",        SkSLType::kHalf4 },
        { "yuvToRGBMatrix",        SkSLType::kHalf3x3 },
        { "yuvToRGBTranslate",     SkSLType::kFloat3 },
};

static constexpr Uniform kCubicYUVImageShaderUniforms[] = {
        { "invImgSizeY",           SkSLType::kFloat2 },
        { "invImgSizeUV",          SkSLType::kFloat2 },  // Relative to Y's texel space
        { "subset",                SkSLType::kFloat4 },
        { "tilemodeX",             SkSLType::kInt },
        { "tilemodeY",             SkSLType::kInt },
        { "cubicCoeffs",           SkSLType::kHalf4x4 },
        { "channelSelectY",        SkSLType::kHalf4 },
        { "channelSelectU",        SkSLType::kHalf4 },
        { "channelSelectV",        SkSLType::kHalf4 },
        { "channelSelectA",        SkSLType::kHalf4 },
        { "yuvToRGBMatrix",        SkSLType::kHalf3x3 },
        { "yuvToRGBTranslate",     SkSLType::kFloat3 },
};

static constexpr Uniform kHWYUVImageShaderUniforms[] = {
        { "invImgSizeY",           SkSLType::kFloat2 },
        { "invImgSizeUV",          SkSLType::kFloat2 },  // Relative to Y's texel space
        { "channelSelectY",        SkSLType::kHalf4 },
        { "channelSelectU",        SkSLType::kHalf4 },
        { "channelSelectV",        SkSLType::kHalf4 },
        { "channelSelectA",        SkSLType::kHalf4 },
        { "yuvToRGBMatrix",        SkSLType::kHalf3x3 },
        { "yuvToRGBTranslate",     SkSLType::kFloat3 },
};

static constexpr TextureAndSampler kYUVISTexturesAndSamplers[] = {
    { "samplerY" },
    { "samplerU" },
    { "samplerV" },
    { "samplerA" },
};

static constexpr char kYUVImageShaderName[] = "sk_yuv_image_shader";
static constexpr char kCubicYUVImageShaderName[] = "sk_cubic_yuv_image_shader";
static constexpr char kHWYUVImageShaderName[] = "sk_hw_yuv_image_shader";

//--------------------------------------------------------------------------------------------------
static constexpr Uniform kCoordClampShaderUniforms[] = {
        { "subset", SkSLType::kFloat4 },
};

static constexpr char kCoordClampShaderName[] = "CoordClamp";

static constexpr int kNumCoordClampShaderChildren = 1;

// Create a helper function that clamps the local coords to the subset, invokes the child
// entry with those updated coordinates, and returns the result. This helper function meets the
// requirements for use with GenerateDefaultExpression, so there's no need to have a separate
// special GenerateCoordClampExpression.
// TODO: this has a lot of overlap with GenerateLocalMatrixPreamble
std::string GenerateCoordClampPreamble(const ShaderInfo& shaderInfo,
                                       const ShaderNode* node) {
    SkASSERT(node->codeSnippetId() == (int) BuiltInCodeSnippetID::kCoordClampShader);
    SkASSERT(node->numChildren() == kNumCoordClampShaderChildren);

    // Get the child's evaluation expression.
    static constexpr char kUnusedDestColor[] = "half4(1)";
    std::string childExpr = emit_expression_for_entry(shaderInfo, node->child(0),
                                                      {"inColor", kUnusedDestColor, "coords"});

    std::string subsetUni =
            get_mangled_uniform_name(shaderInfo, node->entry()->fUniforms[0], node->keyIndex());

    std::string helperFnName =
            get_mangled_name(node->entry()->fStaticFunctionName, node->keyIndex());
    return SkSL::String::printf("half4 %s(half4 inColor, half4 destColor, float2 coords) {"
                                    "coords = clamp(coords, %s.LT, %s.RB);"
                                    "return %s;"
                                "}",
                                helperFnName.c_str(),
                                subsetUni.c_str(),
                                subsetUni.c_str(),
                                childExpr.c_str());
}


//--------------------------------------------------------------------------------------------------
static constexpr Uniform kDitherShaderUniforms[] = {
        { "range", SkSLType::kHalf },
};

static constexpr TextureAndSampler kDitherTexturesAndSamplers[] = {
        {"sampler"},
};

static constexpr char kDitherShaderName[] = "sk_dither_shader";

//--------------------------------------------------------------------------------------------------
static constexpr Uniform kPerlinNoiseShaderUniforms[] = {
        { "baseFrequency", SkSLType::kFloat2 },
        { "stitchData",    SkSLType::kFloat2 },
        { "noiseType",     SkSLType::kInt },
        { "numOctaves",    SkSLType::kInt },
        { "stitching",     SkSLType::kInt },
};

static constexpr TextureAndSampler kPerlinNoiseShaderTexturesAndSamplers[] = {
        { "permutationsSampler" },
        { "noiseSampler" },
};

static constexpr char kPerlinNoiseShaderName[] = "perlin_noise_shader";

//--------------------------------------------------------------------------------------------------
static constexpr Uniform CoeffBlendderUniforms[] = {
        { "coeffs", SkSLType::kHalf4 },
};

static constexpr char kCoeffBlenderName[] = "sk_coeff_blend";

//--------------------------------------------------------------------------------------------------
static constexpr Uniform kBlendModeBlenderUniforms[] = {
        { "blendMode", SkSLType::kInt },
};

static constexpr char kBlendModeBlenderName[] = "sk_blend";

//--------------------------------------------------------------------------------------------------
static constexpr int kNumBlendShaderChildren = 3;

std::string GenerateBlendShaderPreamble(const ShaderInfo& shaderInfo,
                                        const ShaderNode* node) {
    // Children are src, dst, and blender
    SkASSERT(node->numChildren() == 3);

    // Create a helper function that invokes the src and dst children, then calls the blend child
    // with the src and dst results.
    std::string helperFn = SkSL::String::printf(
            "half4 %s(half4 inColor, half4 destColor, float2 pos) {",
            get_mangled_name(node->entry()->fStaticFunctionName, node->keyIndex()).c_str());

    // Get src and dst colors.
    const ShaderSnippet::Args args = {"inColor", "destColor", "pos"};
    std::string srcVar = emit_glue_code_for_entry(shaderInfo, node->child(0), args, &helperFn);
    std::string dstVar = emit_glue_code_for_entry(shaderInfo, node->child(1), args, &helperFn);

    // Do the blend.
    static constexpr char kUnusedLocalCoords[] = "float2(0)";

    std::string blendResultVar = emit_glue_code_for_entry(
            shaderInfo, node->child(2), {srcVar, dstVar, kUnusedLocalCoords}, &helperFn);

    SkSL::String::appendf(&helperFn,
                              "return %s;"
                          "}",
                          blendResultVar.c_str());
    return helperFn;
}

//--------------------------------------------------------------------------------------------------
class GraphitePipelineCallbacks : public SkSL::PipelineStage::Callbacks {
public:
    GraphitePipelineCallbacks(const ShaderInfo& shaderInfo,
                              const ShaderNode* node,
                              std::string* preamble,
                              const SkRuntimeEffect* effect)
            : fShaderInfo(shaderInfo)
            , fNode(node)
            , fPreamble(preamble)
            , fEffect(effect) {}

    std::string declareUniform(const SkSL::VarDeclaration* decl) override {
        std::string result = get_mangled_name(std::string(decl->var()->name()), fNode->keyIndex());
        if (fShaderInfo.ssboIndex()) {
            result = EmitStorageBufferAccess("fs", fShaderInfo.ssboIndex(), result.c_str());
        }
        return result;
    }

    void defineFunction(const char* decl, const char* body, bool isMain) override {
        if (isMain) {
            SkSL::String::appendf(
                 fPreamble,
                 "half4 %s(half4 inColor, half4 destColor, float2 coords) {"
                     "%s"
                 "}",
                 get_mangled_name(fNode->entry()->fName, fNode->keyIndex()).c_str(),
                 body);
        } else {
            SkSL::String::appendf(fPreamble, "%s {%s}\n", decl, body);
        }
    }

    void declareFunction(const char* decl) override {
        *fPreamble += std::string(decl);
    }

    void defineStruct(const char* definition) override {
        *fPreamble += std::string(definition);
    }

    void declareGlobal(const char* declaration) override {
        *fPreamble += std::string(declaration);
    }

    std::string sampleShader(int index, std::string coords) override {
        return emit_expression_for_entry(fShaderInfo, fNode->child(index),
                                         {"inColor", "destColor", coords});
    }

    std::string sampleColorFilter(int index, std::string color) override {
        return emit_expression_for_entry(fShaderInfo, fNode->child(index),
                                         {color, "destColor", "coords"});
    }

    std::string sampleBlender(int index, std::string src, std::string dst) override {
        return emit_expression_for_entry(fShaderInfo, fNode->child(index),
                                         {src, dst, "coords"});
    }

    std::string toLinearSrgb(std::string color) override {
        if (!SkRuntimeEffectPriv::UsesColorTransform(fEffect)) {
            return color;
        }

        color = SkSL::String::printf("(%s).rgb1", color.c_str());
        std::string helper = get_mangled_name("toLinearSRGB", fNode->keyIndex());
        std::string xformedColor = SkSL::String::printf("%s(%s)",
                                    helper.c_str(),
                                    color.c_str());
        return SkSL::String::printf("(%s).rgb", xformedColor.c_str());
    }


    std::string fromLinearSrgb(std::string color) override {
        if (!SkRuntimeEffectPriv::UsesColorTransform(fEffect)) {
            return color;
        }

        color = SkSL::String::printf("(%s).rgb1", color.c_str());
        std::string helper = get_mangled_name("fromLinearSRGB", fNode->keyIndex());
        std::string xformedColor = SkSL::String::printf("%s(%s)",
                                                        helper.c_str(),
                                                        color.c_str());
        return SkSL::String::printf("(%s).rgb", xformedColor.c_str());
    }

    std::string getMangledName(const char* name) override {
        return get_mangled_name(name, fNode->keyIndex());
    }

private:
    const ShaderInfo& fShaderInfo;
    const ShaderNode* fNode;
    std::string* fPreamble;
    const SkRuntimeEffect* fEffect;
};

std::string GenerateRuntimeShaderPreamble(const ShaderInfo& shaderInfo,
                                          const ShaderNode* node) {
    // Find this runtime effect in the runtime-effect dictionary.
    SkASSERT(node->codeSnippetId() >= kBuiltInCodeSnippetIDCount);
    const SkRuntimeEffect* effect;
    if (node->codeSnippetId() < kSkiaKnownRuntimeEffectsStart + kStableKeyCnt) {
        effect = GetKnownRuntimeEffect(static_cast<StableKey>(node->codeSnippetId()));
    } else {
        SkASSERT(node->codeSnippetId() >= kUnknownRuntimeEffectIDStart);
        effect = shaderInfo.runtimeEffectDictionary()->find(node->codeSnippetId());
    }
    SkASSERT(effect);

    const SkSL::Program& program = SkRuntimeEffectPriv::Program(*effect);

    std::string preamble;
    if (SkRuntimeEffectPriv::UsesColorTransform(effect)) {
        SkSL::String::appendf(
                &preamble,
                "half4 %s(half4 inColor) {"
                    "return sk_color_space_transform(inColor, %s, %s, %s, %s, %s);"
                "}",
                get_mangled_name("toLinearSRGB", node->keyIndex()).c_str(),
                get_mangled_uniform_name(shaderInfo, kRuntimeEffectColorSpaceTransformUniforms[0],
                                         node->keyIndex()).c_str(),
                get_mangled_uniform_name(shaderInfo, kRuntimeEffectColorSpaceTransformUniforms[1],
                                         node->keyIndex()).c_str(),
                get_mangled_uniform_name(shaderInfo, kRuntimeEffectColorSpaceTransformUniforms[2],
                                         node->keyIndex()).c_str(),
                get_mangled_uniform_name(shaderInfo, kRuntimeEffectColorSpaceTransformUniforms[3],
                                         node->keyIndex()).c_str(),
                get_mangled_uniform_name(shaderInfo, kRuntimeEffectColorSpaceTransformUniforms[4],
                                         node->keyIndex()).c_str());
        SkSL::String::appendf(
                &preamble,
                "half4 %s(half4 inColor) {"
                    "return sk_color_space_transform(inColor, %s, %s, %s, %s, %s);"
                "}",
                get_mangled_name("fromLinearSRGB", node->keyIndex()).c_str(),
                get_mangled_uniform_name(shaderInfo, kRuntimeEffectColorSpaceTransformUniforms[5],
                                         node->keyIndex()).c_str(),
                get_mangled_uniform_name(shaderInfo, kRuntimeEffectColorSpaceTransformUniforms[6],
                                         node->keyIndex()).c_str(),
                get_mangled_uniform_name(shaderInfo, kRuntimeEffectColorSpaceTransformUniforms[7],
                                         node->keyIndex()).c_str(),
                get_mangled_uniform_name(shaderInfo, kRuntimeEffectColorSpaceTransformUniforms[8],
                                         node->keyIndex()).c_str(),
                get_mangled_uniform_name(shaderInfo, kRuntimeEffectColorSpaceTransformUniforms[9],
                                         node->keyIndex()).c_str());
    }

    GraphitePipelineCallbacks callbacks{shaderInfo, node, &preamble, effect};
    SkSL::PipelineStage::ConvertProgram(program, "coords", "inColor", "destColor", &callbacks);
    return preamble;
}

std::string GenerateRuntimeShaderExpression(const ShaderInfo& shaderInfo,
                                            const ShaderNode* node,
                                            const ShaderSnippet::Args& args) {
    return SkSL::String::printf(
            "%s(%.*s, %.*s, %.*s)",
            get_mangled_name(node->entry()->fName, node->keyIndex()).c_str(),
            (int)args.fPriorStageOutput.size(), args.fPriorStageOutput.data(),
            (int)args.fBlenderDstColor.size(),  args.fBlenderDstColor.data(),
            (int)args.fFragCoord.size(),        args.fFragCoord.data());
}

//--------------------------------------------------------------------------------------------------
// TODO: investigate the implications of having separate hlsa and rgba matrix colorfilters. It
// may be that having them separate will not contribute to combinatorial explosion.
static constexpr Uniform kMatrixColorFilterUniforms[] = {
        { "matrix",    SkSLType::kFloat4x4 },
        { "translate", SkSLType::kFloat4 },
        { "inHSL",     SkSLType::kInt },
};

static constexpr char kMatrixColorFilterName[] = "sk_matrix_colorfilter";

//--------------------------------------------------------------------------------------------------
static constexpr char kComposeName[] = "Compose";

static constexpr int kNumComposeChildren = 2;

// Compose two children, assuming the first child is the innermost.
std::string GenerateNestedChildrenPreamble(const ShaderInfo& shaderInfo,
                                           const ShaderNode* node) {
    SkASSERT(node->numChildren() == 2);

    // Evaluate inner child.
    static constexpr char kUnusedDestColor[] = "half4(1)";
    std::string innerColor = emit_expression_for_entry(shaderInfo, node->child(0),
                                                       {"inColor", kUnusedDestColor, "coords"});

    // Evaluate outer child.
    std::string outerColor = emit_expression_for_entry(shaderInfo, node->child(1),
                                                       {innerColor, kUnusedDestColor, "coords"});

    // Create a helper function that invokes the inner expression, then passes that result to the
    // outer expression, and returns the composed result.
    std::string helperFnName = get_mangled_name(node->entry()->fName, node->keyIndex());
    return SkSL::String::printf("half4 %s(half4 inColor, half4 destColor, float2 coords) {"
                                    "return %s;"
                                "}",
                                helperFnName.c_str(),
                                outerColor.c_str());
}

//--------------------------------------------------------------------------------------------------
static constexpr TextureAndSampler kTableColorFilterTexturesAndSamplers[] = {
        {"tableSampler"},
};

static constexpr char kTableColorFilterName[] = "sk_table_colorfilter";

//--------------------------------------------------------------------------------------------------
static constexpr char kGaussianColorFilterName[] = "sk_gaussian_colorfilter";

//--------------------------------------------------------------------------------------------------
static constexpr Uniform kColorSpaceTransformUniforms[] = {
        { "flags",          SkSLType::kInt },
        { "srcKind",        SkSLType::kInt },
        { "gamutTransform", SkSLType::kHalf3x3 },
        { "dstKind",        SkSLType::kInt },
        { "csXformCoeffs",  SkSLType::kHalf4x4 },
};

static_assert(0 == static_cast<int>(skcms_TFType_Invalid),
              "ColorSpaceTransform code depends on skcms_TFType");
static_assert(1 == static_cast<int>(skcms_TFType_sRGBish),
              "ColorSpaceTransform code depends on skcms_TFType");
static_assert(2 == static_cast<int>(skcms_TFType_PQish),
              "ColorSpaceTransform code depends on skcms_TFType");
static_assert(3 == static_cast<int>(skcms_TFType_HLGish),
              "ColorSpaceTransform code depends on skcms_TFType");
static_assert(4 == static_cast<int>(skcms_TFType_HLGinvish),
              "ColorSpaceTransform code depends on skcms_TFType");

// TODO: We can meaningfully check these when we can use C++20 features.
// static_assert(0x1 == SkColorSpaceXformSteps::Flags{.unpremul = true}.mask(),
//               "ColorSpaceTransform code depends on SkColorSpaceXformSteps::Flags");
// static_assert(0x2 == SkColorSpaceXformSteps::Flags{.linearize = true}.mask(),
//               "ColorSpaceTransform code depends on SkColorSpaceXformSteps::Flags");
// static_assert(0x4 == SkColorSpaceXformSteps::Flags{.gamut_transform = true}.mask(),
//               "ColorSpaceTransform code depends on SkColorSpaceXformSteps::Flags");
// static_assert(0x8 == SkColorSpaceXformSteps::Flags{.encode = true}.mask(),
//               "ColorSpaceTransform code depends on SkColorSpaceXformSteps::Flags");
// static_assert(0x10 == SkColorSpaceXformSteps::Flags{.premul = true}.mask(),
//               "ColorSpaceTransform code depends on SkColorSpaceXformSteps::Flags");

static constexpr char kColorSpaceTransformName[] = "sk_color_space_transform";

//--------------------------------------------------------------------------------------------------
static constexpr char kErrorName[] = "sk_error";

//--------------------------------------------------------------------------------------------------
static constexpr char kPassthroughShaderName[] = "sk_passthrough";

//--------------------------------------------------------------------------------------------------

std::string GeneratePrimitiveColorExpression(const ShaderInfo&,
                                             const ShaderNode* node,
                                             const ShaderSnippet::Args&) {
    return "primitiveColor";
}

//--------------------------------------------------------------------------------------------------

} // anonymous namespace

#if defined(SK_DEBUG)
bool ShaderCodeDictionary::isValidID(int snippetID) const {
    if (snippetID < 0) {
        return false;
    }

    if (snippetID < kBuiltInCodeSnippetIDCount) {
        return true;
    }
    if (snippetID >= kSkiaKnownRuntimeEffectsStart && snippetID < kSkiaKnownRuntimeEffectsEnd) {
        return snippetID < kSkiaKnownRuntimeEffectsStart + kStableKeyCnt;
    }

    SkAutoSpinlock lock{fSpinLock};

    if (snippetID >= kUnknownRuntimeEffectIDStart) {
        int userDefinedCodeSnippetID = snippetID - kUnknownRuntimeEffectIDStart;
        return userDefinedCodeSnippetID < SkTo<int>(fUserDefinedCodeSnippets.size());
    }

    return false;
}

void ShaderCodeDictionary::dump(UniquePaintParamsID id) const {
    this->lookup(id).dump(this, id);
}
#endif

#if defined(GRAPHITE_TEST_UTILS)

int ShaderCodeDictionary::addRuntimeEffectSnippet(const char* functionName) {
    SkAutoSpinlock lock{fSpinLock};

    fUserDefinedCodeSnippets.push_back(
            std::make_unique<ShaderSnippet>("UserDefined",
                                            SkSpan<const Uniform>(),            // no uniforms
                                            SnippetRequirementFlags::kNone,
                                            SkSpan<const TextureAndSampler>(),  // no samplers
                                            functionName,
                                            GenerateDefaultExpression,
                                            GenerateDefaultPreamble,
                                            kNoChildren));

    return kUnknownRuntimeEffectIDStart + fUserDefinedCodeSnippets.size() - 1;
}

#endif // GRAPHITE_TEST_UTILS

static SkSLType uniform_type_to_sksl_type(const SkRuntimeEffect::Uniform& u) {
    using Type = SkRuntimeEffect::Uniform::Type;
    if (u.flags & SkRuntimeEffect::Uniform::kHalfPrecision_Flag) {
        switch (u.type) {
            case Type::kFloat:    return SkSLType::kHalf;
            case Type::kFloat2:   return SkSLType::kHalf2;
            case Type::kFloat3:   return SkSLType::kHalf3;
            case Type::kFloat4:   return SkSLType::kHalf4;
            case Type::kFloat2x2: return SkSLType::kHalf2x2;
            case Type::kFloat3x3: return SkSLType::kHalf3x3;
            case Type::kFloat4x4: return SkSLType::kHalf4x4;
            // NOTE: shorts cannot be uniforms, so we shouldn't ever get here.
            // Defensively return the full precision integer type.
            case Type::kInt:      SkDEBUGFAIL("unsupported uniform type"); return SkSLType::kInt;
            case Type::kInt2:     SkDEBUGFAIL("unsupported uniform type"); return SkSLType::kInt2;
            case Type::kInt3:     SkDEBUGFAIL("unsupported uniform type"); return SkSLType::kInt3;
            case Type::kInt4:     SkDEBUGFAIL("unsupported uniform type"); return SkSLType::kInt4;
        }
    } else {
        switch (u.type) {
            case Type::kFloat:    return SkSLType::kFloat;
            case Type::kFloat2:   return SkSLType::kFloat2;
            case Type::kFloat3:   return SkSLType::kFloat3;
            case Type::kFloat4:   return SkSLType::kFloat4;
            case Type::kFloat2x2: return SkSLType::kFloat2x2;
            case Type::kFloat3x3: return SkSLType::kFloat3x3;
            case Type::kFloat4x4: return SkSLType::kFloat4x4;
            case Type::kInt:      return SkSLType::kInt;
            case Type::kInt2:     return SkSLType::kInt2;
            case Type::kInt3:     return SkSLType::kInt3;
            case Type::kInt4:     return SkSLType::kInt4;
        }
    }
    SkUNREACHABLE;
}

const char* ShaderCodeDictionary::addTextToArena(std::string_view text) {
    char* textInArena = fArena.makeArrayDefault<char>(text.size() + 1);
    memcpy(textInArena, text.data(), text.size());
    textInArena[text.size()] = '\0';
    return textInArena;
}

SkSpan<const Uniform> ShaderCodeDictionary::convertUniforms(const SkRuntimeEffect* effect) {
    using rteUniform = SkRuntimeEffect::Uniform;
    SkSpan<const rteUniform> uniforms = effect->uniforms();

    int numBaseUniforms = uniforms.size();
    int xtraUniforms = 0;
    if (SkRuntimeEffectPriv::UsesColorTransform(effect)) {
        xtraUniforms += std::size(kRuntimeEffectColorSpaceTransformUniforms);
    }

    // Convert the SkRuntimeEffect::Uniform array into its Uniform equivalent.
    int numUniforms = numBaseUniforms + xtraUniforms;
    Uniform* uniformArray = fArena.makeInitializedArray<Uniform>(numUniforms, [&](int index) {
        if (index >= numBaseUniforms) {
            return kRuntimeEffectColorSpaceTransformUniforms[index - numBaseUniforms];
        }

        const rteUniform* u;
        u = &uniforms[index];

        // The existing uniform names live in the passed-in SkRuntimeEffect and may eventually
        // disappear. Copy them into fArena. (It's safe to do this within makeInitializedArray; the
        // entire array is allocated in one big slab before any initialization calls are done.)
        const char* name = this->addTextToArena(u->name);

        // Add one Uniform to our array.
        SkSLType type = uniform_type_to_sksl_type(*u);
        return (u->flags & rteUniform::kArray_Flag) ? Uniform(name, type, u->count)
                                                    : Uniform(name, type);
    });

    return SkSpan<const Uniform>(uniformArray, numUniforms);
}

int ShaderCodeDictionary::findOrCreateRuntimeEffectSnippet(const SkRuntimeEffect* effect) {
    SkEnumBitMask<SnippetRequirementFlags> snippetFlags = SnippetRequirementFlags::kNone;
    if (effect->allowShader()) {
        snippetFlags |= SnippetRequirementFlags::kLocalCoords;
    }
    if (effect->allowBlender()) {
        snippetFlags |= SnippetRequirementFlags::kBlenderDstColor;
    }

    SkAutoSpinlock lock{fSpinLock};

    if (int stableKey = SkRuntimeEffectPriv::StableKey(*effect)) {
        SkASSERT(stableKey >= kSkiaKnownRuntimeEffectsStart &&
                 stableKey < kSkiaKnownRuntimeEffectsStart + kStableKeyCnt);

        int index = stableKey - kSkiaKnownRuntimeEffectsStart;

        if (!fKnownRuntimeEffectCodeSnippets[index].fExpressionGenerator) {
            const char* name = get_known_rte_name(static_cast<StableKey>(stableKey));
            fKnownRuntimeEffectCodeSnippets[index] = ShaderSnippet(
                    name,
                    this->convertUniforms(effect),
                    snippetFlags,
                    /* texturesAndSamplers= */ {},
                    name,
                    GenerateRuntimeShaderExpression,
                    GenerateRuntimeShaderPreamble,
                    (int)effect->children().size());
        }

        return stableKey;
    }

    // Use the combination of {SkSL program hash, uniform size} as our key.
    // In the unfortunate event of a hash collision, at least we'll have the right amount of
    // uniform data available.
    RuntimeEffectKey key;
    key.fHash = SkRuntimeEffectPriv::Hash(*effect);
    key.fUniformSize = effect->uniformSize();

    int32_t* existingCodeSnippetID = fRuntimeEffectMap.find(key);
    if (existingCodeSnippetID) {
        return *existingCodeSnippetID;
    }

    // TODO: the memory for user-defined entries could go in the dictionary's arena but that
    // would have to be a thread safe allocation since the arena also stores entries for
    // 'fHash' and 'fEntryVector'
    fUserDefinedCodeSnippets.push_back(
        std::make_unique<ShaderSnippet>("RuntimeEffect",
                                        this->convertUniforms(effect),
                                        snippetFlags,
                                        /* texturesAndSamplers= */SkSpan<const TextureAndSampler>(),
                                        kRuntimeShaderName,
                                        GenerateRuntimeShaderExpression,
                                        GenerateRuntimeShaderPreamble,
                                        (int)effect->children().size()));

    int newCodeSnippetID = kUnknownRuntimeEffectIDStart + fUserDefinedCodeSnippets.size() - 1;

    fRuntimeEffectMap.set(key, newCodeSnippetID);
    return newCodeSnippetID;
}

ShaderCodeDictionary::ShaderCodeDictionary() {
    // The 0th index is reserved as invalid
    fIDToPaintKey.push_back(PaintParamsKey::Invalid());

    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kError] = {
            "Error",
            { },     // no uniforms
            SnippetRequirementFlags::kNone,
            { },     // no samplers
            kErrorName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kPriorOutput] = {
            "PassthroughShader",
            { },     // no uniforms
            SnippetRequirementFlags::kPriorStageOutput,
            { },     // no samplers
            kPassthroughShaderName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kSolidColorShader] = {
            "SolidColor",
            SkSpan(kSolidShaderUniforms),
            SnippetRequirementFlags::kNone,
            { },     // no samplers
            kSolidShaderName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kRGBPaintColor] = {
            "RGBPaintColor",
            SkSpan(kPaintColorUniforms),
            SnippetRequirementFlags::kNone,
            { },     // no samplers
            kRGBPaintColorName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kAlphaOnlyPaintColor] = {
            "AlphaOnlyPaintColor",
            SkSpan(kPaintColorUniforms),
            SnippetRequirementFlags::kNone,
            { },     // no samplers
            kAlphaOnlyPaintColorName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kLinearGradientShader4] = {
            "LinearGradient4",
            SkSpan(kLinearGradientUniforms4),
            SnippetRequirementFlags::kLocalCoords,
            { },     // no samplers
            kLinearGradient4Name,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kLinearGradientShader8] = {
            "LinearGradient8",
            SkSpan(kLinearGradientUniforms8),
            SnippetRequirementFlags::kLocalCoords,
            { },     // no samplers
            kLinearGradient8Name,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kLinearGradientShaderTexture] = {
            "LinearGradientTexture",
            SkSpan(kLinearGradientUniformsTexture),
            SnippetRequirementFlags::kLocalCoords,
            SkSpan(kTextureGradientTexturesAndSamplers),
            kLinearGradientTextureName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kLinearGradientShaderBuffer] = {
            "LinearGradientBuffer",
            SkSpan(kLinearGradientUniformsBuffer),
            SnippetRequirementFlags::kLocalCoords | SnippetRequirementFlags::kGradientBuffer,
            { },     // no samplers
            kLinearGradientBufferName,
            GenerateGradientBufferExpression,
            GenerateGradientBufferPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kRadialGradientShader4] = {
            "RadialGradient4",
            SkSpan(kRadialGradientUniforms4),
            SnippetRequirementFlags::kLocalCoords,
            { },     // no samplers
            kRadialGradient4Name,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kRadialGradientShader8] = {
            "RadialGradient8",
            SkSpan(kRadialGradientUniforms8),
            SnippetRequirementFlags::kLocalCoords,
            { },     // no samplers
            kRadialGradient8Name,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kRadialGradientShaderTexture] = {
            "RadialGradientTexture",
            SkSpan(kRadialGradientUniformsTexture),
            SnippetRequirementFlags::kLocalCoords,
            SkSpan(kTextureGradientTexturesAndSamplers),
            kRadialGradientTextureName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kRadialGradientShaderBuffer] = {
            "RadialGradientBuffer",
            SkSpan(kRadialGradientUniformsBuffer),
            SnippetRequirementFlags::kLocalCoords | SnippetRequirementFlags::kGradientBuffer,
            { },     // no samplers
            kRadialGradientBufferName,
            GenerateGradientBufferExpression,
            GenerateGradientBufferPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kSweepGradientShader4] = {
            "SweepGradient4",
            SkSpan(kSweepGradientUniforms4),
            SnippetRequirementFlags::kLocalCoords,
            { },     // no samplers
            kSweepGradient4Name,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kSweepGradientShader8] = {
            "SweepGradient8",
            SkSpan(kSweepGradientUniforms8),
            SnippetRequirementFlags::kLocalCoords,
            { },     // no samplers
            kSweepGradient8Name,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kSweepGradientShaderTexture] = {
            "SweepGradientTexture",
            SkSpan(kSweepGradientUniformsTexture),
            SnippetRequirementFlags::kLocalCoords,
            SkSpan(kTextureGradientTexturesAndSamplers),
            kSweepGradientTextureName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kSweepGradientShaderBuffer] = {
            "SweepGradientBuffer",
            SkSpan(kSweepGradientUniformsBuffer),
            SnippetRequirementFlags::kLocalCoords | SnippetRequirementFlags::kGradientBuffer,
            { },     // no samplers
            kSweepGradientBufferName,
            GenerateGradientBufferExpression,
            GenerateGradientBufferPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kConicalGradientShader4] = {
            "ConicalGradient4",
            SkSpan(kConicalGradientUniforms4),
            SnippetRequirementFlags::kLocalCoords,
            { },     // no samplers
            kConicalGradient4Name,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kConicalGradientShader8] = {
            "ConicalGradient8",
            SkSpan(kConicalGradientUniforms8),
            SnippetRequirementFlags::kLocalCoords,
            { },     // no samplers
            kConicalGradient8Name,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kConicalGradientShaderTexture] = {
            "ConicalGradientTexture",
            SkSpan(kConicalGradientUniformsTexture),
            SnippetRequirementFlags::kLocalCoords,
            SkSpan(kTextureGradientTexturesAndSamplers),
            kConicalGradientTextureName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kConicalGradientShaderBuffer] = {
            "ConicalGradientBuffer",
            SkSpan(kConicalGradientUniformsBuffer),
            SnippetRequirementFlags::kLocalCoords | SnippetRequirementFlags::kGradientBuffer,
            { },     // no samplers
            kConicalGradientBufferName,
            GenerateGradientBufferExpression,
            GenerateGradientBufferPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kLocalMatrixShader] = {
            "LocalMatrixShader",
            SkSpan(kLocalMatrixShaderUniforms),
            (SnippetRequirementFlags::kPriorStageOutput |
             SnippetRequirementFlags::kLocalCoords),
            { },     // no samplers
            kLocalMatrixShaderName,
            GenerateDefaultExpression,
            GenerateLocalMatrixPreamble,
            kNumLocalMatrixShaderChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kImageShader] = {
            "ImageShader",
            SkSpan(kImageShaderUniforms),
            SnippetRequirementFlags::kLocalCoords,
            SkSpan(kISTexturesAndSamplers),
            kImageShaderName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kCubicImageShader] = {
            "CubicImageShader",
            SkSpan(kCubicImageShaderUniforms),
            SnippetRequirementFlags::kLocalCoords,
            SkSpan(kISTexturesAndSamplers),
            kCubicImageShaderName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kHWImageShader] = {
            "HardwareImageShader",
            SkSpan(kHWImageShaderUniforms),
            SnippetRequirementFlags::kLocalCoords,
            SkSpan(kISTexturesAndSamplers),
            kHWImageShaderName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kYUVImageShader] = {
            "YUVImageShader",
            SkSpan(kYUVImageShaderUniforms),
            SnippetRequirementFlags::kLocalCoords,
            SkSpan(kYUVISTexturesAndSamplers),
            kYUVImageShaderName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kCubicYUVImageShader] = {
            "CubicYUVImageShader",
            SkSpan(kCubicYUVImageShaderUniforms),
            SnippetRequirementFlags::kLocalCoords,
            SkSpan(kYUVISTexturesAndSamplers),
            kCubicYUVImageShaderName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kHWYUVImageShader] = {
            "HWYUVImageShader",
            SkSpan(kHWYUVImageShaderUniforms),
            SnippetRequirementFlags::kLocalCoords,
            SkSpan(kYUVISTexturesAndSamplers),
            kHWYUVImageShaderName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kCoordClampShader] = {
            "CoordClampShader",
            SkSpan(kCoordClampShaderUniforms),
            SnippetRequirementFlags::kLocalCoords,
            { },     // no samplers
            kCoordClampShaderName,
            GenerateDefaultExpression,
            GenerateCoordClampPreamble,
            kNumCoordClampShaderChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kDitherShader] = {
            "DitherShader",
            SkSpan(kDitherShaderUniforms),
            (SnippetRequirementFlags::kPriorStageOutput | SnippetRequirementFlags::kLocalCoords),
            SkSpan(kDitherTexturesAndSamplers),
            kDitherShaderName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kPerlinNoiseShader] = {
            "PerlinNoiseShader",
            SkSpan(kPerlinNoiseShaderUniforms),
            SnippetRequirementFlags::kLocalCoords,
            SkSpan(kPerlinNoiseShaderTexturesAndSamplers),
            kPerlinNoiseShaderName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    // SkColorFilter snippets
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kMatrixColorFilter] = {
            "MatrixColorFilter",
            SkSpan(kMatrixColorFilterUniforms),
            SnippetRequirementFlags::kPriorStageOutput,
            { },     // no samplers
            kMatrixColorFilterName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kTableColorFilter] = {
            "TableColorFilter",
            { },     // no uniforms
            SnippetRequirementFlags::kPriorStageOutput,
            SkSpan(kTableColorFilterTexturesAndSamplers),
            kTableColorFilterName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kGaussianColorFilter] = {
            "GaussianColorFilter",
            { },     // no uniforms
            SnippetRequirementFlags::kPriorStageOutput,
            { },     // no samplers
            kGaussianColorFilterName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kColorSpaceXformColorFilter] = {
            "ColorSpaceTransform",
            SkSpan(kColorSpaceTransformUniforms),
            SnippetRequirementFlags::kPriorStageOutput,
            { },     // no samplers
            kColorSpaceTransformName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };

    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kBlendShader] = {
            "BlendShader",
            { },     // no uniforms
            SnippetRequirementFlags::kNone,
            { },     // no samplers
            "BlendShader",
            GenerateDefaultExpression,
            GenerateBlendShaderPreamble,
            kNumBlendShaderChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kCoeffBlender] = {
            "CoeffBlender",
            SkSpan(CoeffBlendderUniforms),
            SnippetRequirementFlags::kPriorStageOutput | SnippetRequirementFlags::kBlenderDstColor,
            { },     // no samplers
            kCoeffBlenderName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kBlendModeBlender] = {
            "BlendModeBlender",
            SkSpan(kBlendModeBlenderUniforms),
            SnippetRequirementFlags::kPriorStageOutput | SnippetRequirementFlags::kBlenderDstColor,
            { },     // no samplers
            kBlendModeBlenderName,
            GenerateDefaultExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };

    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kPrimitiveColor] = {
            "PrimitiveColor",
            { },                // no uniforms
            SnippetRequirementFlags::kNone,
            { },                // no samplers
            "primitive color",  // no static sksl
            GeneratePrimitiveColorExpression,
            GenerateDefaultPreamble,
            kNoChildren
    };

    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kDstReadSample] = {
            "DstReadSample",
            SkSpan(kDstReadSampleUniforms),
            SnippetRequirementFlags::kSurfaceColor,
            SkSpan(kDstReadSampleTexturesAndSamplers),
            "InitSurfaceColor",
            GenerateDstReadSampleExpression,
            GenerateDstReadSamplePreamble,
            kNoChildren
    };
    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kDstReadFetch] = {
            "DstReadFetch",
            { },     // no uniforms
            SnippetRequirementFlags::kSurfaceColor,
            { },     // no samplers
            "InitSurfaceColor",
            GenerateDstReadFetchExpression,
            GenerateDstReadFetchPreamble,
            kNoChildren
    };

    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kClipShader] = {
            "ClipShader",
            { },            // no uniforms
            SnippetRequirementFlags::kNone,
            { },            // no samplers
            "clip shader",  // no static sksl
            GenerateClipShaderExpression,
            GenerateClipShaderPreamble,
            kNumClipShaderChildren
    };

    fBuiltInCodeSnippets[(int) BuiltInCodeSnippetID::kCompose] = {
            "Compose",
            { },     // no uniforms
            SnippetRequirementFlags::kPriorStageOutput,
            { },     // no samplers
            kComposeName,
            GenerateDefaultExpression,
            GenerateNestedChildrenPreamble,
            kNumComposeChildren
    };

    // Fixed-function blend mode snippets are all the same, their functionality is entirely defined
    // by their unique code snippet IDs.
    for (int i = 0; i <= (int) SkBlendMode::kLastCoeffMode; ++i) {
        int ffBlendModeID = kFixedFunctionBlendModeIDOffset + i;
        fBuiltInCodeSnippets[ffBlendModeID] = {
                SkBlendMode_Name(static_cast<SkBlendMode>(i)),
                { },     // no uniforms
                SnippetRequirementFlags::kPriorStageOutput |
                SnippetRequirementFlags::kBlenderDstColor,
                { },     // no samplers
                skgpu::BlendFuncName(static_cast<SkBlendMode>(i)),
                GenerateDefaultExpression,
                GenerateDefaultPreamble,
                kNoChildren
        };
    }
}

// Verify that the built-in code IDs for fixed function blending are consistent with SkBlendMode.
// clang-format off
static_assert((int)SkBlendMode::kClear    == (int)BuiltInCodeSnippetID::kFixedFunctionClearBlendMode    - kFixedFunctionBlendModeIDOffset);
static_assert((int)SkBlendMode::kSrc      == (int)BuiltInCodeSnippetID::kFixedFunctionSrcBlendMode      - kFixedFunctionBlendModeIDOffset);
static_assert((int)SkBlendMode::kDst      == (int)BuiltInCodeSnippetID::kFixedFunctionDstBlendMode      - kFixedFunctionBlendModeIDOffset);
static_assert((int)SkBlendMode::kSrcOver  == (int)BuiltInCodeSnippetID::kFixedFunctionSrcOverBlendMode  - kFixedFunctionBlendModeIDOffset);
static_assert((int)SkBlendMode::kDstOver  == (int)BuiltInCodeSnippetID::kFixedFunctionDstOverBlendMode  - kFixedFunctionBlendModeIDOffset);
static_assert((int)SkBlendMode::kSrcIn    == (int)BuiltInCodeSnippetID::kFixedFunctionSrcInBlendMode    - kFixedFunctionBlendModeIDOffset);
static_assert((int)SkBlendMode::kDstIn    == (int)BuiltInCodeSnippetID::kFixedFunctionDstInBlendMode    - kFixedFunctionBlendModeIDOffset);
static_assert((int)SkBlendMode::kSrcOut   == (int)BuiltInCodeSnippetID::kFixedFunctionSrcOutBlendMode   - kFixedFunctionBlendModeIDOffset);
static_assert((int)SkBlendMode::kDstOut   == (int)BuiltInCodeSnippetID::kFixedFunctionDstOutBlendMode   - kFixedFunctionBlendModeIDOffset);
static_assert((int)SkBlendMode::kSrcATop  == (int)BuiltInCodeSnippetID::kFixedFunctionSrcATopBlendMode  - kFixedFunctionBlendModeIDOffset);
static_assert((int)SkBlendMode::kDstATop  == (int)BuiltInCodeSnippetID::kFixedFunctionDstATopBlendMode  - kFixedFunctionBlendModeIDOffset);
static_assert((int)SkBlendMode::kXor      == (int)BuiltInCodeSnippetID::kFixedFunctionXorBlendMode      - kFixedFunctionBlendModeIDOffset);
static_assert((int)SkBlendMode::kPlus     == (int)BuiltInCodeSnippetID::kFixedFunctionPlusBlendMode     - kFixedFunctionBlendModeIDOffset);
static_assert((int)SkBlendMode::kModulate == (int)BuiltInCodeSnippetID::kFixedFunctionModulateBlendMode - kFixedFunctionBlendModeIDOffset);
static_assert((int)SkBlendMode::kScreen   == (int)BuiltInCodeSnippetID::kFixedFunctionScreenBlendMode   - kFixedFunctionBlendModeIDOffset);
// clang-format on

} // namespace skgpu::graphite
