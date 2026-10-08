// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// libSceJson, natively: sce::Json::Value, String, Parser and the rest the
// title imports.
//
// The bridge's version kept its values in a managed table and handed out
// strings from a guest allocator that this runtime does not provide, so
// String::c_str() returned null and the title's loader thread read through
// it right after the PS Studios intro: the thread died, and the game sat on
// a black screen from then on.
//
// Layout, from what the title's code does with these objects:
//   Value, 0x20 bytes: +0x00 the node it stands for, +0x08 whether it owns
//     that node, +0x10 the scalar that getBoolean/getInteger/getUInteger/
//     getReal hand back a reference to, +0x1C the type.
//   String, 8 bytes (the title keeps one in an eight-byte stack slot with
//     its stack guard right above): a pointer to a NUL-terminated buffer.
// Guest code runs in this address space, so host memory is guest memory.

#ifndef PS5RT_JSON_H
#define PS5RT_JSON_H

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ps5rt_json {

enum ValueType : std::int32_t {
    kNull = 0,
    kBoolean = 1,
    kInteger = 2,
    kUInteger = 3,
    kReal = 4,
    kString = 5,
    kArray = 6,
    kObject = 7,
};

struct Node;

// The guest-visible object.
struct GuestValue {
    Node* node;
    std::uint64_t owned;
    std::uint64_t scalar;
    std::uint32_t reserved;
    std::int32_t type;
};
static_assert(sizeof(GuestValue) == 0x20);

struct Node {
    ValueType type = kNull;
    bool boolean = false;
    std::int64_t integer = 0;
    std::uint64_t uinteger = 0;
    double real = 0.0;
    std::string text;
    std::vector<Node*> elements;
    std::vector<std::pair<std::string, Node*>> members;
    // What operator[] hands out for this node: a Value that refers to it
    // without owning it, made once and kept for as long as the node.
    GuestValue* handle = nullptr;
};

inline void clear_children(Node& node);

inline void destroy(Node* node) {
    if (node == nullptr) {
        return;
    }
    clear_children(*node);
    delete node->handle;
    delete node;
}

inline void clear_children(Node& node) {
    for (auto* element : node.elements) {
        destroy(element);
    }
    for (auto& [key, member] : node.members) {
        (void)key;
        destroy(member);
    }
    node.elements.clear();
    node.members.clear();
}

inline void reset(Node& node, ValueType type) {
    clear_children(node);
    node.type = type;
    node.boolean = false;
    node.integer = 0;
    node.uinteger = 0;
    node.real = 0.0;
    node.text.clear();
}

inline Node* clone(const Node* source) {
    auto* copy = new Node();
    if (source == nullptr) {
        return copy;
    }
    copy->type = source->type;
    copy->boolean = source->boolean;
    copy->integer = source->integer;
    copy->uinteger = source->uinteger;
    copy->real = source->real;
    copy->text = source->text;
    for (const auto* element : source->elements) {
        copy->elements.push_back(clone(element));
    }
    for (const auto& [key, member] : source->members) {
        copy->members.emplace_back(key, clone(member));
    }
    return copy;
}

// The value every missing key and out-of-range index answers with. Never
// written: set() on it is ignored.
inline Node& null_node() {
    static Node node;
    return node;
}

inline void mirror(GuestValue& value) {
    const Node* node = value.node != nullptr ? value.node : &null_node();
    value.type = node->type;
    std::uint64_t bits = 0;
    switch (node->type) {
    case kBoolean:
        bits = node->boolean ? 1 : 0;
        break;
    case kInteger:
        std::memcpy(&bits, &node->integer, sizeof(bits));
        break;
    case kUInteger:
        bits = node->uinteger;
        break;
    case kReal:
        std::memcpy(&bits, &node->real, sizeof(bits));
        break;
    default:
        break;
    }
    value.scalar = bits;
}

inline GuestValue* handle_for(Node& node) {
    if (node.handle == nullptr) {
        node.handle = new GuestValue{&node, 0, 0, 0, 0};
    }
    mirror(*node.handle);
    return node.handle;
}

inline GuestValue* null_handle() {
    static GuestValue value{&null_node(), 0, 0, 0, kNull};
    return &value;
}

// The node a Value stands for, made if it has none yet - a Value the title
// built without one of our constructors, zeroed by it or never touched.
inline Node* writable_node(GuestValue* value) {
    if (value == nullptr) {
        return nullptr;
    }
    if (value->node == &null_node()) {
        return nullptr;
    }
    if (value->node == nullptr) {
        value->node = new Node();
        value->owned = 1;
    }
    return value->node;
}

inline const Node& readable_node(const GuestValue* value) {
    return value != nullptr && value->node != nullptr ? *value->node
                                                      : null_node();
}

inline bool trace_enabled() {
    static const bool enabled = [] {
        const auto* value = std::getenv("PS5RT_TRACE_JSON");
        return value != nullptr && value[0] == '1';
    }();
    return enabled;
}

// ---- parsing ----

class Reader {
public:
    Reader(const char* text, std::size_t size) : text_(text), size_(size) {}

    Node* document() {
        skip_space();
        auto* root = value(0);
        if (root == nullptr) {
            return nullptr;
        }
        skip_space();
        // A buffer may carry its terminator in the size it was given.
        while (position_ < size_ && text_[position_] == '\0') {
            ++position_;
        }
        skip_space();
        if (position_ != size_) {
            destroy(root);
            return nullptr;
        }
        return root;
    }

private:
    void skip_space() {
        while (position_ < size_) {
            const auto c = text_[position_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++position_;
            } else {
                break;
            }
        }
    }

    bool literal(std::string_view word) {
        if (size_ - position_ < word.size() ||
            std::string_view(text_ + position_, word.size()) != word) {
            return false;
        }
        position_ += word.size();
        return true;
    }

    Node* value(int depth) {
        if (depth > 512 || position_ >= size_) {
            return nullptr;
        }
        const auto c = text_[position_];
        if (c == '{') {
            return object(depth);
        }
        if (c == '[') {
            return array(depth);
        }
        if (c == '"') {
            auto* node = new Node();
            node->type = kString;
            if (!string(node->text)) {
                destroy(node);
                return nullptr;
            }
            return node;
        }
        if (literal("true") || literal("false")) {
            auto* node = new Node();
            node->type = kBoolean;
            node->boolean = c == 't';
            return node;
        }
        if (literal("null")) {
            return new Node();
        }
        return number();
    }

    Node* object(int depth) {
        ++position_;
        auto* node = new Node();
        node->type = kObject;
        skip_space();
        if (position_ < size_ && text_[position_] == '}') {
            ++position_;
            return node;
        }
        while (true) {
            skip_space();
            std::string key;
            if (position_ >= size_ || text_[position_] != '"' ||
                !string(key)) {
                destroy(node);
                return nullptr;
            }
            skip_space();
            if (position_ >= size_ || text_[position_] != ':') {
                destroy(node);
                return nullptr;
            }
            ++position_;
            skip_space();
            auto* member = value(depth + 1);
            if (member == nullptr) {
                destroy(node);
                return nullptr;
            }
            node->members.emplace_back(std::move(key), member);
            skip_space();
            if (position_ < size_ && text_[position_] == ',') {
                ++position_;
                continue;
            }
            if (position_ < size_ && text_[position_] == '}') {
                ++position_;
                return node;
            }
            destroy(node);
            return nullptr;
        }
    }

    Node* array(int depth) {
        ++position_;
        auto* node = new Node();
        node->type = kArray;
        skip_space();
        if (position_ < size_ && text_[position_] == ']') {
            ++position_;
            return node;
        }
        while (true) {
            skip_space();
            auto* element = value(depth + 1);
            if (element == nullptr) {
                destroy(node);
                return nullptr;
            }
            node->elements.push_back(element);
            skip_space();
            if (position_ < size_ && text_[position_] == ',') {
                ++position_;
                continue;
            }
            if (position_ < size_ && text_[position_] == ']') {
                ++position_;
                return node;
            }
            destroy(node);
            return nullptr;
        }
    }

    static void append_utf8(std::string& out, std::uint32_t code) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    bool hex4(std::uint32_t& code) {
        if (size_ - position_ < 4) {
            return false;
        }
        code = 0;
        for (int index = 0; index < 4; ++index) {
            const auto c = text_[position_++];
            code <<= 4;
            if (c >= '0' && c <= '9') {
                code |= static_cast<std::uint32_t>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                code |= static_cast<std::uint32_t>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                code |= static_cast<std::uint32_t>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    bool string(std::string& out) {
        ++position_;
        while (position_ < size_) {
            const auto c = text_[position_++];
            if (c == '"') {
                return true;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (position_ >= size_) {
                return false;
            }
            const auto escape = text_[position_++];
            switch (escape) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint32_t code = 0;
                if (!hex4(code)) {
                    return false;
                }
                if (code >= 0xD800 && code < 0xDC00 &&
                    size_ - position_ >= 6 && text_[position_] == '\\' &&
                    text_[position_ + 1] == 'u') {
                    position_ += 2;
                    std::uint32_t low = 0;
                    if (!hex4(low)) {
                        return false;
                    }
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                append_utf8(out, code);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    Node* number() {
        const auto start = position_;
        if (position_ < size_ && text_[position_] == '-') {
            ++position_;
        }
        bool fraction = false;
        bool digits = false;
        while (position_ < size_) {
            const auto c = text_[position_];
            if (c >= '0' && c <= '9') {
                digits = true;
            } else if (c == '.' || c == 'e' || c == 'E' ||
                       ((c == '+' || c == '-') &&
                        (text_[position_ - 1] == 'e' ||
                         text_[position_ - 1] == 'E'))) {
                fraction = true;
            } else {
                break;
            }
            ++position_;
        }
        if (!digits) {
            return nullptr;
        }
        const std::string token(text_ + start, position_ - start);
        auto* node = new Node();
        char* end = nullptr;
        if (!fraction) {
            errno = 0;
            const auto integer = std::strtoll(token.c_str(), &end, 10);
            if (errno == 0 && end != nullptr && *end == '\0') {
                node->type = kInteger;
                node->integer = integer;
                return node;
            }
            if (token[0] != '-') {
                errno = 0;
                const auto uinteger = std::strtoull(token.c_str(), &end, 10);
                if (errno == 0 && end != nullptr && *end == '\0') {
                    node->type = kUInteger;
                    node->uinteger = uinteger;
                    return node;
                }
            }
        }
        node->type = kReal;
        node->real = std::strtod(token.c_str(), &end);
        return node;
    }

    const char* text_;
    std::size_t size_;
    std::size_t position_ = 0;
};

// ---- writing ----

inline void write_string(std::string& out, const std::string& text) {
    out += '"';
    for (const auto c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char escaped[8] = {};
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                out += escaped;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

inline void write(std::string& out, const Node& node) {
    char number[64] = {};
    switch (node.type) {
    case kNull: out += "null"; break;
    case kBoolean: out += node.boolean ? "true" : "false"; break;
    case kInteger:
        std::snprintf(number, sizeof(number), "%lld",
                      static_cast<long long>(node.integer));
        out += number;
        break;
    case kUInteger:
        std::snprintf(number, sizeof(number), "%llu",
                      static_cast<unsigned long long>(node.uinteger));
        out += number;
        break;
    case kReal:
        std::snprintf(number, sizeof(number), "%.17g", node.real);
        out += number;
        break;
    case kString: write_string(out, node.text); break;
    case kArray:
        out += '[';
        for (std::size_t index = 0; index < node.elements.size(); ++index) {
            if (index != 0) {
                out += ',';
            }
            write(out, *node.elements[index]);
        }
        out += ']';
        break;
    case kObject:
        out += '{';
        for (std::size_t index = 0; index < node.members.size(); ++index) {
            if (index != 0) {
                out += ',';
            }
            write_string(out, node.members[index].first);
            out += ':';
            write(out, *node.members[index].second);
        }
        out += '}';
        break;
    }
}

// ---- String ----

struct GuestString {
    char* text;
};
static_assert(sizeof(GuestString) == 8);

inline void string_assign(GuestString* string, const char* text,
                          std::size_t length) {
    if (string == nullptr) {
        return;
    }
    auto* copy = static_cast<char*>(std::malloc(length + 1));
    if (copy == nullptr) {
        return;
    }
    if (length != 0) {
        std::memcpy(copy, text, length);
    }
    copy[length] = '\0';
    std::free(string->text);
    string->text = copy;
}

// Puts a new scalar into a Value, keeping its node where it has one: a
// Value handed out by operator[] stands for a node inside its parent.
template <typename Fill>
inline void set_value(GuestValue* value, ValueType type, Fill fill) {
    auto* node = writable_node(value);
    if (node == nullptr) {
        return;
    }
    reset(*node, type);
    fill(*node);
    mirror(*value);
}

inline void value_construct(GuestValue* value) {
    if (value == nullptr) {
        return;
    }
    value->node = new Node();
    value->owned = 1;
    value->scalar = 0;
    value->reserved = 0;
    value->type = kNull;
}

}  // namespace ps5rt_json

namespace pj = ps5rt_json;

extern "C" {

// MemAllocator, Initializer: nothing to set up here.
PS5RT_GUEST_ABI void* ps5rt_json_this(void* self) { return self; }
PS5RT_GUEST_ABI void ps5rt_json_nothing(void*) {}
PS5RT_GUEST_ABI std::int32_t ps5rt_json_initialize(void*, const void*) {
    return 0;
}

PS5RT_GUEST_ABI void* ps5rt_json_value_ctor(pj::GuestValue* value) {
    pj::value_construct(value);
    return value;
}
PS5RT_GUEST_ABI void* ps5rt_json_value_ctor_bool(pj::GuestValue* value,
                                                 bool boolean) {
    pj::value_construct(value);
    pj::set_value(value, pj::kBoolean,
                  [&](pj::Node& node) { node.boolean = boolean; });
    return value;
}
PS5RT_GUEST_ABI void* ps5rt_json_value_ctor_long(pj::GuestValue* value,
                                                 std::int64_t integer) {
    pj::value_construct(value);
    pj::set_value(value, pj::kInteger,
                  [&](pj::Node& node) { node.integer = integer; });
    return value;
}
PS5RT_GUEST_ABI void* ps5rt_json_value_ctor_ulong(pj::GuestValue* value,
                                                  std::uint64_t uinteger) {
    pj::value_construct(value);
    pj::set_value(value, pj::kUInteger,
                  [&](pj::Node& node) { node.uinteger = uinteger; });
    return value;
}
PS5RT_GUEST_ABI void* ps5rt_json_value_ctor_double(pj::GuestValue* value,
                                                   double real) {
    pj::value_construct(value);
    pj::set_value(value, pj::kReal,
                  [&](pj::Node& node) { node.real = real; });
    return value;
}
PS5RT_GUEST_ABI void* ps5rt_json_value_ctor_cstr(pj::GuestValue* value,
                                                 const char* text) {
    pj::value_construct(value);
    pj::set_value(value, pj::kString, [&](pj::Node& node) {
        node.text = text != nullptr ? text : "";
    });
    return value;
}
PS5RT_GUEST_ABI void* ps5rt_json_value_ctor_string(
    pj::GuestValue* value, const pj::GuestString* string) {
    pj::value_construct(value);
    pj::set_value(value, pj::kString, [&](pj::Node& node) {
        node.text = string != nullptr && string->text != nullptr
            ? string->text : "";
    });
    return value;
}

PS5RT_GUEST_ABI void ps5rt_json_value_dtor(pj::GuestValue* value) {
    if (value == nullptr) {
        return;
    }
    if (value->owned != 0 && value->node != &pj::null_node()) {
        pj::destroy(value->node);
    }
    value->node = nullptr;
    value->owned = 0;
}

PS5RT_GUEST_ABI std::int32_t ps5rt_json_set_bool(pj::GuestValue* value,
                                                 bool boolean) {
    pj::set_value(value, pj::kBoolean,
                  [&](pj::Node& node) { node.boolean = boolean; });
    return 0;
}
PS5RT_GUEST_ABI std::int32_t ps5rt_json_set_long(pj::GuestValue* value,
                                                 std::int64_t integer) {
    pj::set_value(value, pj::kInteger,
                  [&](pj::Node& node) { node.integer = integer; });
    return 0;
}
PS5RT_GUEST_ABI std::int32_t ps5rt_json_set_ulong(pj::GuestValue* value,
                                                  std::uint64_t uinteger) {
    pj::set_value(value, pj::kUInteger,
                  [&](pj::Node& node) { node.uinteger = uinteger; });
    return 0;
}
PS5RT_GUEST_ABI std::int32_t ps5rt_json_set_double(pj::GuestValue* value,
                                                   double real) {
    pj::set_value(value, pj::kReal,
                  [&](pj::Node& node) { node.real = real; });
    return 0;
}
PS5RT_GUEST_ABI std::int32_t ps5rt_json_set_cstr(pj::GuestValue* value,
                                                 const char* text) {
    pj::set_value(value, pj::kString, [&](pj::Node& node) {
        node.text = text != nullptr ? text : "";
    });
    return 0;
}
PS5RT_GUEST_ABI std::int32_t ps5rt_json_set_string(
    pj::GuestValue* value, const pj::GuestString* string) {
    pj::set_value(value, pj::kString, [&](pj::Node& node) {
        node.text = string != nullptr && string->text != nullptr
            ? string->text : "";
    });
    return 0;
}
PS5RT_GUEST_ABI std::int32_t ps5rt_json_set_type(pj::GuestValue* value,
                                                 std::int32_t type) {
    if (type < pj::kNull || type > pj::kObject) {
        type = pj::kNull;
    }
    pj::set_value(value, static_cast<pj::ValueType>(type),
                  [](pj::Node&) {});
    return 0;
}

PS5RT_GUEST_ABI void* ps5rt_json_value_assign(pj::GuestValue* value,
                                              const pj::GuestValue* source) {
    if (value == nullptr || value == source) {
        return value;
    }
    auto* copy = pj::clone(&pj::readable_node(source));
    auto* node = pj::writable_node(value);
    if (node == nullptr) {
        pj::destroy(copy);
        return value;
    }
    // Into the node already there, so a Value that stands for a member of
    // a tree changes that member.
    pj::reset(*node, copy->type);
    node->boolean = copy->boolean;
    node->integer = copy->integer;
    node->uinteger = copy->uinteger;
    node->real = copy->real;
    node->text = std::move(copy->text);
    node->elements = std::move(copy->elements);
    node->members = std::move(copy->members);
    copy->elements.clear();
    copy->members.clear();
    pj::destroy(copy);
    pj::mirror(*value);
    return value;
}

PS5RT_GUEST_ABI std::int32_t ps5rt_json_get_type(pj::GuestValue* value) {
    if (value != nullptr) {
        pj::mirror(*value);
    }
    return pj::readable_node(value).type;
}

PS5RT_GUEST_ABI std::uint64_t ps5rt_json_count(const pj::GuestValue* value) {
    const auto& node = pj::readable_node(value);
    return node.type == pj::kArray ? node.elements.size()
        : node.type == pj::kObject ? node.members.size()
        : 0;
}

// getBoolean, getInteger, getUInteger, getReal: a reference to the scalar.
PS5RT_GUEST_ABI const void* ps5rt_json_get_scalar(pj::GuestValue* value) {
    if (value == nullptr) {
        return &pj::null_handle()->scalar;
    }
    pj::mirror(*value);
    return &value->scalar;
}

PS5RT_GUEST_ABI const void* ps5rt_json_index_key(const pj::GuestValue* value,
                                                 const char* key) {
    const auto& node = pj::readable_node(value);
    if (node.type == pj::kObject && key != nullptr) {
        for (const auto& [name, member] : node.members) {
            if (name == key) {
                return pj::handle_for(*member);
            }
        }
    }
    if (pj::trace_enabled()) {
        std::fprintf(stderr, "json.index_missing key=%s type=%d\n",
                     key != nullptr ? key : "(null)", node.type);
    }
    return pj::null_handle();
}

PS5RT_GUEST_ABI const void* ps5rt_json_index_position(
    const pj::GuestValue* value, std::uint64_t position) {
    const auto& node = pj::readable_node(value);
    if (node.type == pj::kArray && position < node.elements.size()) {
        return pj::handle_for(*node.elements[position]);
    }
    return pj::null_handle();
}

PS5RT_GUEST_ABI std::int32_t ps5rt_json_to_string(
    const pj::GuestValue* value, pj::GuestString* string) {
    const auto& node = pj::readable_node(value);
    std::string text;
    if (node.type == pj::kString) {
        text = node.text;
    } else {
        pj::write(text, node);
    }
    pj::string_assign(string, text.data(), text.size());
    return 0;
}

PS5RT_GUEST_ABI void* ps5rt_json_string_ctor(pj::GuestString* string) {
    if (string != nullptr) {
        string->text = nullptr;
        pj::string_assign(string, "", 0);
    }
    return string;
}
PS5RT_GUEST_ABI void* ps5rt_json_string_ctor_cstr(pj::GuestString* string,
                                                  const char* text) {
    if (string != nullptr) {
        string->text = nullptr;
        const auto* source = text != nullptr ? text : "";
        pj::string_assign(string, source, std::strlen(source));
    }
    return string;
}
PS5RT_GUEST_ABI void* ps5rt_json_string_copy(pj::GuestString* string,
                                             const pj::GuestString* source) {
    if (string != nullptr) {
        string->text = nullptr;
        const auto* text = source != nullptr && source->text != nullptr
            ? source->text : "";
        pj::string_assign(string, text, std::strlen(text));
    }
    return string;
}
PS5RT_GUEST_ABI void ps5rt_json_string_dtor(pj::GuestString* string) {
    if (string != nullptr) {
        std::free(string->text);
        string->text = nullptr;
    }
}
PS5RT_GUEST_ABI const char* ps5rt_json_string_c_str(
    const pj::GuestString* string) {
    return string != nullptr && string->text != nullptr ? string->text : "";
}

PS5RT_GUEST_ABI std::int32_t ps5rt_json_parse(pj::GuestValue* value,
                                              const char* text,
                                              std::uint64_t size) {
    constexpr std::int32_t kInvalidToken = static_cast<std::int32_t>(0x80920101u);
    constexpr std::int32_t kEmptyBuffer = static_cast<std::int32_t>(0x80920105u);
    if (value == nullptr || text == nullptr || size == 0) {
        return kEmptyBuffer;
    }
    pj::Reader reader(text, static_cast<std::size_t>(size));
    auto* root = reader.document();
    if (pj::trace_enabled()) {
        std::fprintf(stderr, "json.parse bytes=%llu ok=%d head=%.60s\n",
                     static_cast<unsigned long long>(size),
                     root != nullptr ? 1 : 0, text);
    }
    if (root == nullptr) {
        return kInvalidToken;
    }
    pj::GuestValue source{root, 1, 0, 0, root->type};
    ps5rt_json_value_assign(value, &source);
    pj::destroy(root);
    return 0;
}

// Which handler a libSceJson NID goes to, or null for one this does not
// implement. PS5RT_JSON_BRIDGE=1 leaves them all to the bridge.
void* ps5rt_json_handler(const char* nid, std::size_t length) {
    static const bool bridge = [] {
        const auto* value = std::getenv("PS5RT_JSON_BRIDGE");
        return value != nullptr && value[0] == '1';
    }();
    if (bridge) {
        return nullptr;
    }
    const std::string_view name(nid, length);
    struct Entry {
        std::string_view nid;
        void* handler;
    };
    static const Entry entries[] = {
        {"-hJRce8wn1U", reinterpret_cast<void*>(&ps5rt_json_this)},     // MemAllocator()
        {"OcAgPxcq5Vk", reinterpret_cast<void*>(&ps5rt_json_nothing)},  // ~MemAllocator
        {"cK6bYHf-Q5E", reinterpret_cast<void*>(&ps5rt_json_this)},     // Initializer()
        {"RujUxbr3haM", reinterpret_cast<void*>(&ps5rt_json_nothing)},  // ~Initializer
        {"Cxwy7wHq4J0", reinterpret_cast<void*>(&ps5rt_json_initialize)},
        {"qBMjqyBn3OM", reinterpret_cast<void*>(&ps5rt_json_value_ctor)},
        {"-wa17B7TGnw", reinterpret_cast<void*>(&ps5rt_json_value_ctor)},
        {"UeuWT+yNdCQ", reinterpret_cast<void*>(&ps5rt_json_value_ctor_bool)},
        {"0lLK8+kDqmE", reinterpret_cast<void*>(&ps5rt_json_value_ctor_long)},
        {"x4AUdbhpRB0", reinterpret_cast<void*>(&ps5rt_json_value_ctor_ulong)},
        {"sOmU4vnx3s0", reinterpret_cast<void*>(&ps5rt_json_value_ctor_double)},
        {"b9V6fmppLXY", reinterpret_cast<void*>(&ps5rt_json_value_ctor_cstr)},
        {"sZIoMRGO+jk", reinterpret_cast<void*>(&ps5rt_json_value_ctor_string)},
        {"WTtYf+cNnXI", reinterpret_cast<void*>(&ps5rt_json_value_dtor)},
        {"0eUrW9JAxM0", reinterpret_cast<void*>(&ps5rt_json_value_dtor)},
        {"5yHuiWXo2gg", reinterpret_cast<void*>(&ps5rt_json_set_bool)},
        {"QxVVYhP-mvg", reinterpret_cast<void*>(&ps5rt_json_set_long)},
        {"SIe1ZmW7e7s", reinterpret_cast<void*>(&ps5rt_json_set_ulong)},
        {"BSmWDIkV4w4", reinterpret_cast<void*>(&ps5rt_json_set_double)},
        {"n6FC+l9DU70", reinterpret_cast<void*>(&ps5rt_json_set_cstr)},
        {"6l3Bv2gysNc", reinterpret_cast<void*>(&ps5rt_json_set_string)},
        {"IKQimvG9Wqs", reinterpret_cast<void*>(&ps5rt_json_set_type)},
        {"4zrm6VrgIAw", reinterpret_cast<void*>(&ps5rt_json_value_assign)},
        {"SHtAad20YYM", reinterpret_cast<void*>(&ps5rt_json_get_type)},
        {"RBw+4NukeGQ", reinterpret_cast<void*>(&ps5rt_json_count)},
        {"zTwZdI8AZ5Y", reinterpret_cast<void*>(&ps5rt_json_get_scalar)},
        {"DIxvoy7Ngvk", reinterpret_cast<void*>(&ps5rt_json_get_scalar)},
        {"sn4HNCtNRzY", reinterpret_cast<void*>(&ps5rt_json_get_scalar)},
        {"3qrge7L-AU4", reinterpret_cast<void*>(&ps5rt_json_get_scalar)},
        {"HwDt5lD9Bfo", reinterpret_cast<void*>(&ps5rt_json_index_key)},
        {"XlWbvieLj2M", reinterpret_cast<void*>(&ps5rt_json_index_position)},
        {"0YqYAoO-+Uo", reinterpret_cast<void*>(&ps5rt_json_index_position)},
        {"Ncel8t2Rrpc", reinterpret_cast<void*>(&ps5rt_json_to_string)},
        {"qSmqLXXCPas", reinterpret_cast<void*>(&ps5rt_json_string_ctor)},
        {"eG9E9M6XvTM", reinterpret_cast<void*>(&ps5rt_json_string_ctor)},
        {"9KUZFjI1IxA", reinterpret_cast<void*>(&ps5rt_json_string_ctor_cstr)},
        {"0CAesfH963Q", reinterpret_cast<void*>(&ps5rt_json_string_copy)},
        {"cG1VE2HMl6c", reinterpret_cast<void*>(&ps5rt_json_string_dtor)},
        {"Ui7YFnSTCBw", reinterpret_cast<void*>(&ps5rt_json_string_dtor)},
        {"L1KAkYWml-M", reinterpret_cast<void*>(&ps5rt_json_string_c_str)},
        {"S5JxQnoGF3E", reinterpret_cast<void*>(&ps5rt_json_parse)},
    };
    for (const auto& entry : entries) {
        if (entry.nid == name) {
            return entry.handler;
        }
    }
    return nullptr;
}

}  // extern "C"

#endif  // PS5RT_JSON_H
