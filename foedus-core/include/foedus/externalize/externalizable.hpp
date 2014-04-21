/*
 * Copyright (c) 2014, Hewlett-Packard Development Company, LP.
 * The license and distribution terms for this file are placed in LICENSE.txt.
 */
#ifndef FOEDUS_EXTERNALIZE_EXTERNALIZABLE_HPP_
#define FOEDUS_EXTERNALIZE_EXTERNALIZABLE_HPP_
#include <foedus/cxx11.hpp>
#include <foedus/error_stack.hpp>
#include <stdint.h>
#include <iosfwd>
#include <string>
#include <vector>
// forward declarations for tinyxml2. They should provide a header file for this...
namespace tinyxml2 {
    class XMLDocument;
    class XMLElement;
    class XMLAttribute;
    class XMLComment;
    class XMLNode;
    class XMLText;
    class XMLDeclaration;
    class XMLUnknown;
    class XMLPrinter;
}  // namespace tinyxml2

namespace foedus {
namespace externalize {
/**
 * @brief Represents an object that can be written to and read from files/bytes in XML format.
 * @ingroup EXTERNALIZE
 * @details
 * Derived classes must implement load() and save().
 */
struct Externalizable {
    virtual ~Externalizable() {}

    /**
     * @brief Reads the content of this object from the given XML element.
     * @param[in] element the XML element that represents this object
     * @details
     * Expect errors due to missing-elements, out-of-range values, etc.
     */
    virtual ErrorStack load(tinyxml2::XMLElement* element) = 0;

    /**
     * @brief Writes the content of this object to the given XML element.
     * @param[in] element the XML element that represents this object
     * @details
     * Expect only out-of-memory error.
     * We receive the XML element this object will represent, so this method does not determine
     * the XML element name of itself. The parent object determines children's tag names
     * because one parent object might have multiple child objects of the same type with different
     * XML element name.
     */
    virtual ErrorStack save(tinyxml2::XMLElement* element) const = 0;

    /**
     * Invokes save() and directs the resulting XML text to the given stream.
     */
    void save_to_stream(std::ostream* ptr) const;

    // convenience methods
    static ErrorStack insert_comment(tinyxml2::XMLElement* element, const std::string& comment);
    static ErrorStack create_element(tinyxml2::XMLElement* parent, const std::string& name,
                                    tinyxml2::XMLElement** out);

    static ErrorStack add_element(tinyxml2::XMLElement* parent, const std::string& tag,
                                  const std::string& comment, bool value);
    static ErrorStack add_element(tinyxml2::XMLElement* parent, const std::string& tag,
                                  const std::string& comment, int32_t value);
    static ErrorStack add_element(tinyxml2::XMLElement* parent, const std::string& tag,
                                  const std::string& comment, uint32_t value);
    static ErrorStack add_element(tinyxml2::XMLElement* parent, const std::string& tag,
                                  const std::string& comment, const std::string& value);
    static ErrorStack add_element(tinyxml2::XMLElement* parent, const std::string& tag,
                    const std::string& comment, const std::vector< std::string >& value);
    static ErrorStack add_element(tinyxml2::XMLElement* parent, const std::string& tag,
                                  const std::string& comment, float value);
    static ErrorStack add_element(tinyxml2::XMLElement* parent, const std::string& tag,
                                  const std::string& comment, double value);

    template <typename ENUM>
    static ErrorStack add_enum_element(tinyxml2::XMLElement* parent, const std::string& tag,
                                const std::string& comment, ENUM value) {
        return add_element(parent, tag, comment, static_cast<int32_t>(value));
    }

    static ErrorStack add_child_element(tinyxml2::XMLElement* parent, const std::string& tag,
                                  const std::string& comment, const Externalizable& child);

    static ErrorStack get_element(tinyxml2::XMLElement* parent, const std::string& tag,
                                bool* out, bool optional = false, bool default_value = false);

    // Tinyxml2 doesn't support int64_t in QueryXxxText and SetText, so up to 32 bit.
    static ErrorStack get_element(tinyxml2::XMLElement* parent, const std::string& tag,
                               int32_t* out, bool optional = false, int32_t default_value = 0);
    static ErrorStack get_element(tinyxml2::XMLElement* parent, const std::string& tag,
                               uint32_t* out, bool optional = false, uint32_t default_value = 0);

    template <typename T, typename LARGEST_TYPE>
    static ErrorStack get_smaller_element(tinyxml2::XMLElement* parent, const std::string& tag,
                               T* out, bool optional, T default_value) {
        LARGEST_TYPE tmp;
        CHECK_ERROR(get_element(parent, tag, &tmp, optional, default_value));
        if (static_cast<LARGEST_TYPE>(static_cast<T>(tmp)) != tmp) {
            return ERROR_STACK_MSG(ERROR_CODE_CONF_VALUE_OUTOFRANGE, tag.c_str());
        }
        *out = static_cast<T>(tmp);
        return RET_OK;
    }
    static ErrorStack get_element(tinyxml2::XMLElement* parent, const std::string& tag,
                               int16_t* out, bool optional = false, int16_t default_value = 0) {
        return get_smaller_element<int16_t, int32_t>(parent, tag, out, optional, default_value);
    }
    static ErrorStack get_element(tinyxml2::XMLElement* parent, const std::string& tag,
                               int8_t* out, bool optional = false, int8_t default_value = 0) {
        return get_smaller_element<int8_t, int32_t>(parent, tag, out, optional, default_value);
    }
    static ErrorStack get_element(tinyxml2::XMLElement* parent, const std::string& tag,
                               uint16_t* out, bool optional = false, uint16_t default_value = 0) {
        return get_smaller_element<uint16_t, uint32_t>(parent, tag, out, optional, default_value);
    }
    static ErrorStack get_element(tinyxml2::XMLElement* parent, const std::string& tag,
                               uint8_t* out, bool optional = false, uint8_t default_value = 0) {
        return get_smaller_element<uint8_t, uint32_t>(parent, tag, out, optional, default_value);
    }

    template <typename ENUM>
    static ErrorStack get_enum_element(tinyxml2::XMLElement* parent, const std::string& tag,
                    ENUM* out, bool optional = false, ENUM default_value = static_cast<ENUM>(0)) {
        // enum might be signged or unsigned, 1 byte, 2 byte, or 4 byte.
        // But surely it won't exceed int32_t range.
        return get_smaller_element<ENUM, int32_t>(parent, tag, out, optional, default_value);
    }

    static ErrorStack get_element(tinyxml2::XMLElement* parent, const std::string& tag,
                               float* out, bool optional = false, float default_value = 0);
    static ErrorStack get_element(tinyxml2::XMLElement* parent, const std::string& tag,
                               double* out, bool optional = false, double default_value = 0);
    static ErrorStack get_element(tinyxml2::XMLElement* parent, const std::string& tag,
                        std::string* out, bool optional = false, const char* default_value = "");
    static ErrorStack get_element(tinyxml2::XMLElement* parent, const std::string& tag,
                        std::vector< std::string >* out, bool optional = false);

    static ErrorStack get_child_element(tinyxml2::XMLElement* parent, const std::string& tag,
                        Externalizable* child, bool optional = false);
};

}  // namespace externalize
}  // namespace foedus

// A bit tricky to get "a" from a in C macro.
#define EX_QUOTE(str) #str
#define EX_EXPAND(str) EX_QUOTE(str)

#define EXTERNALIZE_SAVE_ELEMENT(element, attribute, comment) \
    CHECK_ERROR(add_element(element, EX_EXPAND(attribute), comment, attribute))
#define EXTERNALIZE_SAVE_ENUM_ELEMENT(element, attribute, comment) \
    CHECK_ERROR(add_enum_element(element, EX_EXPAND(attribute), comment, attribute))

#define EXTERNALIZE_LOAD_ELEMENT(element, attribute) \
    CHECK_ERROR(get_element(element, EX_EXPAND(attribute), & attribute))
#define EXTERNALIZE_LOAD_ELEMENT_OPTIONAL(element, attribute, default_value) \
    CHECK_ERROR(get_element(element, EX_EXPAND(attribute), & attribute, true, default_value))
#define EXTERNALIZE_LOAD_ENUM_ELEMENT(element, attribute) \
    CHECK_ERROR(get_enum_element(element, EX_EXPAND(attribute), & attribute))
#define EXTERNALIZE_LOAD_ENUM_ELEMENT_OPTIONAL(element, attribute, default_value) \
    CHECK_ERROR(get_enum_element(element, EX_EXPAND(attribute), & attribute, true, default_value))

#endif  // FOEDUS_EXTERNALIZE_EXTERNALIZABLE_HPP_