// A small XML reader and writer for the fence files: elements that hold text or child elements, with no attributes.

#pragma once

#include <string>
#include <vector>

namespace nofences::xml {

struct Element {
    std::string name;
    // The character data of an element that holds text.
    std::string text;
    std::vector<Element> children;

    // The first child with this name, or nullptr.
    const Element* child(const std::string& childName) const;
};

// Parses the document's root element. Throws std::runtime_error when the text isn't well formed.
Element parse(const std::string& document);

// Writes the element as an indented document, with its declaration.
std::string write(const Element& root);

}  // namespace nofences::xml
