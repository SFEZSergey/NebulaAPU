// Copyright (C) 2026 SharpEmu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// A SPIR-V module writer, ported from SpirvModuleBuilder.cs.
//
// The shader translation lives entirely in C#: the guest's compute shaders
// reach SPIR-V through Gen5SpirvTranslator, called from here by way of
// ps5gpu_compile_spirv. That is the part of the frame nothing else can
// reach - 62 of the 104 seconds a run spends on compute is the GPU running
// those shaders, and one of them measures 129517 instructions, 40% of them
// access chains and loads and stores against an emulated register file.
// Moving the translation to native code is what lets that be worked on.
//
// This is the bottom of it and knows nothing about the PS5: it allocates
// ids, holds the module's sections in order, dedupes types and constants,
// and serialises. Ported first because its correctness can be judged on
// its own - the same calls in the same order must produce the same words
// as the C# builder, byte for byte.

#ifndef PS5_SPIRV_BUILDER_H
#define PS5_SPIRV_BUILDER_H

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ps5spirv {

enum class Op : std::uint16_t {
    Nop = 0,
    Name = 5,
    Extension = 10,
    ExtInstImport = 11,
    ExtInst = 12,
    MemoryModel = 14,
    EntryPoint = 15,
    ExecutionMode = 16,
    Capability = 17,
    TypeVoid = 19,
    TypeBool = 20,
    TypeInt = 21,
    TypeFloat = 22,
    TypeVector = 23,
    TypeImage = 25,
    TypeSampler = 26,
    TypeSampledImage = 27,
    TypeArray = 28,
    TypeRuntimeArray = 29,
    TypeStruct = 30,
    TypePointer = 32,
    TypeFunction = 33,
    ConstantTrue = 41,
    ConstantFalse = 42,
    Constant = 43,
    ConstantComposite = 44,
    ConstantNull = 46,
    Function = 54,
    FunctionParameter = 55,
    FunctionEnd = 56,
    FunctionCall = 57,
    Variable = 59,
    Load = 61,
    Store = 62,
    AccessChain = 65,
    ArrayLength = 68,
    Decorate = 71,
    MemberDecorate = 72,
    VectorExtractDynamic = 77,
    VectorInsertDynamic = 78,
    VectorShuffle = 79,
    CompositeConstruct = 80,
    CompositeExtract = 81,
    CompositeInsert = 82,
    CopyObject = 83,
    SampledImage = 86,
    // The two instructions that make control flow structured: a loop names
    // where it merges and where it continues before its terminator, a
    // selection names where its arms rejoin. Without them a module is a
    // graph, which SPIR-V does not accept.
    LoopMerge = 246,
    SelectionMerge = 247,
    Label = 248,
    Branch = 249,
    BranchConditional = 250,
    Switch = 251,
    Return = 253,
    ReturnValue = 254,
    Unreachable = 255,
};

enum class StorageClass : std::uint32_t {
    UniformConstant = 0,
    Input = 1,
    Uniform = 2,
    Output = 3,
    Workgroup = 4,
    Private = 6,
    Function = 7,
    PushConstant = 9,
    StorageBuffer = 12,
};

enum class Capability : std::uint32_t {
    Matrix = 0,
    Shader = 1,
    Float16 = 9,
    Float64 = 10,
    Int64 = 11,
    Int16 = 22,
    Int8 = 39,
    // Reading or writing a storage image whose format is Unknown. A
    // module that does either without declaring these is not valid, and a
    // driver is free to do anything with it.
    StorageImageMultisample = 27,
    ImageCubeArray = 34,
    SampledCubeArray = 35,
    StorageImageReadWithoutFormat = 55,
    StorageImageWriteWithoutFormat = 56,
};

enum class ExecutionModel : std::uint32_t {
    Vertex = 0,
    Fragment = 4,
    GLCompute = 5,
};

enum class ExecutionMode : std::uint32_t {
    OriginUpperLeft = 7,
    LocalSize = 17,
};

enum class ImageDim : std::uint32_t {
    Dim1D = 0,
    Dim2D = 1,
    Dim3D = 2,
    Cube = 3,
};

// The formats a storage image can declare. A sampled image declares
// Unknown, because its format comes from the view rather than the shader.
enum class ImageFormat : std::uint32_t {
    Unknown = 0,
    Rgba32f = 1,
    Rgba16f = 2,
    R32f = 3,
    Rgba8 = 4,
    Rgba8Snorm = 5,
    Rgba32i = 21,
    Rgba16i = 22,
    R32i = 24,
    Rgba32ui = 30,
    Rgba16ui = 31,
    R32ui = 33,
};

// The built-ins a graphics stage needs. Position is read by the
// rasteriser by name rather than by location.
enum class BuiltIn : std::uint32_t {
    Position = 0,
    PointSize = 1,
    FragCoord = 15,
    FrontFacing = 17,
    FragDepth = 22,
    // Which invocation of a workgroup this is, as one number. A wave's
    // lane index is the same thing when each invocation is one lane.
    LocalInvocationIndex = 29,
    // Which workgroup of the dispatch, and which invocation within it, as
    // three numbers each - what a PS5 compute shader finds in its system
    // SGPRs and in v0 to v2.
    WorkgroupId = 26,
    LocalInvocationId = 27,
    // Which vertex and which instance this invocation is drawing. The PS5
    // hands a vertex shader these in registers rather than as inputs, so
    // they are read here and put where the shader expects to find them.
    VertexIndex = 42,
    InstanceIndex = 43,
};

enum class Decoration : std::uint32_t {
    Block = 2,
    ArrayStride = 6,
    BuiltIn = 11,
    // Where a value crosses between stages. The number is the attribute
    // number the hardware used, kept unchanged - the two stages are
    // translated separately and agree only because both read it from the
    // same place.
    Location = 30,
    NonWritable = 24,
    Binding = 33,
    DescriptorSet = 34,
    Offset = 35,
};

// Builds one module. Sections are held apart and written in the order the
// specification fixes, because a module whose capabilities follow its types
// is not a module.
class ModuleBuilder {
public:
    std::uint32_t allocate_id() { return next_id_++; }

    void add_capability(Capability capability) {
        if (declared_capabilities_.insert(capability).second) {
            emit(capabilities_,
                 Op::Capability,
                 {static_cast<std::uint32_t>(capability)});
        }
    }

    void add_extension(const std::string& extension) {
        emit_with_string(extensions_, Op::Extension, {}, extension);
    }

    std::uint32_t import_ext_inst(const std::string& name) {
        const auto existing = ext_inst_imports_.find(name);
        if (existing != ext_inst_imports_.end()) {
            return existing->second;
        }
        const auto id = allocate_id();
        emit_with_string(imports_, Op::ExtInstImport, {id}, name);
        ext_inst_imports_.emplace(name, id);
        return id;
    }

    void set_logical_glsl450_memory_model() {
        emit(memory_model_, Op::MemoryModel, {0, 1});
    }

    void add_entry_point(
        ExecutionModel model,
        std::uint32_t function,
        const std::string& name,
        const std::vector<std::uint32_t>& interfaces) {
        std::vector<std::uint32_t> prefix;
        prefix.reserve(2 + interfaces.size());
        prefix.push_back(static_cast<std::uint32_t>(model));
        prefix.push_back(function);
        for (const auto id : interfaces) {
            prefix.push_back(id);
        }
        // The name sits after the model and the function and before the
        // interface ids, so the tail has to be put back after the string.
        emit_with_string(entry_points_, Op::EntryPoint, prefix, name, 2);
    }

    void add_execution_mode(
        std::uint32_t function,
        ExecutionMode mode,
        const std::vector<std::uint32_t>& operands = {}) {
        std::vector<std::uint32_t> values;
        values.reserve(2 + operands.size());
        values.push_back(function);
        values.push_back(static_cast<std::uint32_t>(mode));
        values.insert(values.end(), operands.begin(), operands.end());
        emit(execution_modes_, Op::ExecutionMode, values);
    }

    void add_name(std::uint32_t target, const std::string& name) {
        emit_with_string(debug_, Op::Name, {target}, name);
    }

    void add_decoration(
        std::uint32_t target,
        Decoration decoration,
        const std::vector<std::uint32_t>& operands = {}) {
        std::vector<std::uint32_t> values;
        values.reserve(2 + operands.size());
        values.push_back(target);
        values.push_back(static_cast<std::uint32_t>(decoration));
        values.insert(values.end(), operands.begin(), operands.end());
        emit(annotations_, Op::Decorate, values);
    }

    void add_member_decoration(
        std::uint32_t structure,
        std::uint32_t member,
        Decoration decoration,
        const std::vector<std::uint32_t>& operands = {}) {
        std::vector<std::uint32_t> values;
        values.reserve(3 + operands.size());
        values.push_back(structure);
        values.push_back(member);
        values.push_back(static_cast<std::uint32_t>(decoration));
        values.insert(values.end(), operands.begin(), operands.end());
        emit(annotations_, Op::MemberDecorate, values);
    }

    // --- types, deduped ---------------------------------------------------

    std::uint32_t type_void() {
        if (void_type_ == 0) {
            void_type_ = allocate_id();
            emit(types_, Op::TypeVoid, {void_type_});
        }
        return void_type_;
    }

    std::uint32_t type_bool() {
        if (bool_type_ == 0) {
            bool_type_ = allocate_id();
            emit(types_, Op::TypeBool, {bool_type_});
        }
        return bool_type_;
    }

    std::uint32_t type_int(std::uint32_t width, bool is_signed) {
        const auto key = std::make_pair(width, is_signed);
        const auto existing = integer_types_.find(key);
        if (existing != integer_types_.end()) {
            return existing->second;
        }
        const auto id = allocate_id();
        emit(types_,
             Op::TypeInt,
             {id, width, is_signed ? 1u : 0u});
        integer_types_.emplace(key, id);
        return id;
    }

    std::uint32_t type_float(std::uint32_t width) {
        const auto existing = float_types_.find(width);
        if (existing != float_types_.end()) {
            return existing->second;
        }
        const auto id = allocate_id();
        emit(types_, Op::TypeFloat, {id, width});
        float_types_.emplace(width, id);
        return id;
    }

    std::uint32_t type_vector(std::uint32_t component, std::uint32_t count) {
        const auto key = std::make_pair(component, count);
        const auto existing = vector_types_.find(key);
        if (existing != vector_types_.end()) {
            return existing->second;
        }
        const auto id = allocate_id();
        emit(types_, Op::TypeVector, {id, component, count});
        vector_types_.emplace(key, id);
        return id;
    }

    std::uint32_t type_pointer(StorageClass storage, std::uint32_t type) {
        const auto key = std::make_pair(
            static_cast<std::uint32_t>(storage), type);
        const auto existing = pointer_types_.find(key);
        if (existing != pointer_types_.end()) {
            return existing->second;
        }
        const auto id = allocate_id();
        emit(types_,
             Op::TypePointer,
             {id, static_cast<std::uint32_t>(storage), type});
        pointer_types_.emplace(key, id);
        return id;
    }

    std::uint32_t type_array(std::uint32_t element, std::uint32_t length_id) {
        const auto key = std::make_pair(element, length_id);
        const auto existing = array_types_.find(key);
        if (existing != array_types_.end()) {
            return existing->second;
        }
        const auto id = allocate_id();
        emit(types_, Op::TypeArray, {id, element, length_id});
        array_types_.emplace(key, id);
        return id;
    }

    std::uint32_t type_runtime_array(std::uint32_t element) {
        const auto existing = runtime_array_types_.find(element);
        if (existing != runtime_array_types_.end()) {
            return existing->second;
        }
        const auto id = allocate_id();
        emit(types_, Op::TypeRuntimeArray, {id, element});
        runtime_array_types_.emplace(element, id);
        return id;
    }

    std::uint32_t type_function(
        std::uint32_t return_type,
        const std::vector<std::uint32_t>& parameters = {}) {
        std::string key = std::to_string(return_type);
        for (const auto parameter : parameters) {
            key += ',';
            key += std::to_string(parameter);
        }
        const auto existing = function_types_.find(key);
        if (existing != function_types_.end()) {
            return existing->second;
        }
        const auto id = allocate_id();
        std::vector<std::uint32_t> operands{id, return_type};
        operands.insert(
            operands.end(), parameters.begin(), parameters.end());
        emit(types_, Op::TypeFunction, operands);
        function_types_.emplace(key, id);
        return id;
    }

    // An image type. The sampled operand is what separates a texture from
    // a storage image - 1 means it is read through a sampler, 2 means it is
    // read and written directly - and a declaration with the wrong one
    // binds against a descriptor of the other kind.
    std::uint32_t type_image(
        std::uint32_t component_type,
        ImageDim dim,
        bool depth,
        bool arrayed,
        bool multisampled,
        std::uint32_t sampled,
        ImageFormat format) {
        const auto key = std::make_tuple(
            component_type,
            static_cast<std::uint32_t>(dim) | (depth ? 0x100u : 0u) |
                (arrayed ? 0x200u : 0u) | (multisampled ? 0x400u : 0u),
            sampled,
            static_cast<std::uint32_t>(format));
        const auto existing = image_types_.find(key);
        if (existing != image_types_.end()) {
            return existing->second;
        }
        const auto id = allocate_id();
        emit(types_,
             Op::TypeImage,
             {id, component_type, static_cast<std::uint32_t>(dim),
              depth ? 1u : 0u, arrayed ? 1u : 0u, multisampled ? 1u : 0u,
              sampled, static_cast<std::uint32_t>(format)});
        image_types_.emplace(key, id);
        return id;
    }

    std::uint32_t type_sampled_image(std::uint32_t image_type) {
        const auto existing = sampled_image_types_.find(image_type);
        if (existing != sampled_image_types_.end()) {
            return existing->second;
        }
        const auto id = allocate_id();
        emit(types_, Op::TypeSampledImage, {id, image_type});
        sampled_image_types_.emplace(image_type, id);
        return id;
    }

    // --- constants, deduped ----------------------------------------------

    std::uint32_t constant(std::uint32_t type, std::uint64_t value) {
        const auto key = std::make_pair(type, value);
        const auto existing = constants_.find(key);
        if (existing != constants_.end()) {
            return existing->second;
        }
        const auto id = allocate_id();
        const auto low = static_cast<std::uint32_t>(value & 0xFFFFFFFFull);
        const auto high = static_cast<std::uint32_t>(value >> 32);
        // A boolean has no OpConstant: it is true or false by opcode. A
        // module written the other way fails validation, and a driver is
        // free to do anything with it.
        if (type != 0 && type == bool_type_) {
            emit(types_, value != 0 ? Op::ConstantTrue : Op::ConstantFalse,
                 {type, id});
        } else if (high != 0) {
            emit(types_, Op::Constant, {type, id, low, high});
        } else {
            emit(types_, Op::Constant, {type, id, low});
        }
        constants_.emplace(key, id);
        return id;
    }

    // --- instructions -----------------------------------------------------

    void add_global(Op opcode, const std::vector<std::uint32_t>& operands) {
        emit(types_, opcode, operands);
    }

    void add_function_word(Op opcode,
                           const std::vector<std::uint32_t>& operands) {
        emit(functions_, opcode, operands);
    }

    // --- serialisation ----------------------------------------------------

    std::vector<std::uint32_t> build() {
        if (memory_model_.empty()) {
            set_logical_glsl450_memory_model();
        }
        std::vector<std::uint32_t> words;
        words.reserve(
            5 + capabilities_.size() + extensions_.size() + imports_.size() +
            memory_model_.size() + entry_points_.size() +
            execution_modes_.size() + debug_.size() + annotations_.size() +
            types_.size() + functions_.size());
        words.push_back(kMagic);
        words.push_back(kVersion15);
        words.push_back(kGenerator);
        words.push_back(next_id_);
        words.push_back(0);
        for (const auto* section : {
                 &capabilities_, &extensions_, &imports_, &memory_model_,
                 &entry_points_, &execution_modes_, &debug_, &annotations_,
                 &types_, &functions_}) {
            words.insert(words.end(), section->begin(), section->end());
        }
        return words;
    }

private:
    static constexpr std::uint32_t kMagic = 0x07230203u;
    static constexpr std::uint32_t kVersion15 = 0x00010500u;
    static constexpr std::uint32_t kGenerator = 0x53504500u;  // "SPE"

    static void emit(
        std::vector<std::uint32_t>& section,
        Op opcode,
        const std::vector<std::uint32_t>& operands) {
        section.push_back(
            (static_cast<std::uint32_t>(operands.size() + 1) << 16) |
            static_cast<std::uint32_t>(opcode));
        section.insert(section.end(), operands.begin(), operands.end());
    }

    // SPIR-V strings are UTF-8, null terminated, padded to a word. A string
    // that fills its last word exactly still gets a whole word of zeroes,
    // because the terminator has to be there.
    static std::vector<std::uint32_t> encode_string(const std::string& value) {
        std::vector<std::uint32_t> words;
        std::uint32_t word = 0;
        int filled = 0;
        for (const auto character : value) {
            word |= static_cast<std::uint32_t>(
                        static_cast<unsigned char>(character))
                << (8 * filled);
            if (++filled == 4) {
                words.push_back(word);
                word = 0;
                filled = 0;
            }
        }
        words.push_back(word);
        return words;
    }

    static void emit_with_string(
        std::vector<std::uint32_t>& section,
        Op opcode,
        const std::vector<std::uint32_t>& prefix,
        const std::string& value,
        int string_before_tail_count = -1) {
        const auto encoded = encode_string(value);
        std::vector<std::uint32_t> operands;
        if (string_before_tail_count < 0) {
            operands = prefix;
            operands.insert(operands.end(), encoded.begin(), encoded.end());
        } else {
            const auto head =
                static_cast<std::size_t>(string_before_tail_count);
            operands.insert(
                operands.end(), prefix.begin(), prefix.begin() + head);
            operands.insert(operands.end(), encoded.begin(), encoded.end());
            operands.insert(
                operands.end(), prefix.begin() + head, prefix.end());
        }
        emit(section, opcode, operands);
    }

    std::vector<std::uint32_t> capabilities_;
    std::vector<std::uint32_t> extensions_;
    std::vector<std::uint32_t> imports_;
    std::vector<std::uint32_t> memory_model_;
    std::vector<std::uint32_t> entry_points_;
    std::vector<std::uint32_t> execution_modes_;
    std::vector<std::uint32_t> debug_;
    std::vector<std::uint32_t> annotations_;
    std::vector<std::uint32_t> types_;
    std::vector<std::uint32_t> functions_;

    std::map<std::pair<std::uint32_t, bool>, std::uint32_t> integer_types_;
    std::map<std::uint32_t, std::uint32_t> float_types_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t>
        vector_types_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t>
        pointer_types_;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t>
        array_types_;
    std::map<std::uint32_t, std::uint32_t> runtime_array_types_;
    std::map<std::string, std::uint32_t> function_types_;
    std::map<
        std::tuple<std::uint32_t, std::uint32_t, std::uint32_t,
                   std::uint32_t>,
        std::uint32_t>
        image_types_;
    std::map<std::uint32_t, std::uint32_t> sampled_image_types_;
    std::map<std::pair<std::uint32_t, std::uint64_t>, std::uint32_t>
        constants_;
    std::set<Capability> declared_capabilities_;
    std::map<std::string, std::uint32_t> ext_inst_imports_;

    std::uint32_t next_id_ = 1;
    std::uint32_t void_type_ = 0;
    std::uint32_t bool_type_ = 0;
};

}  // namespace ps5spirv

#endif  // PS5_SPIRV_BUILDER_H
