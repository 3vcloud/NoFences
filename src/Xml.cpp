#include "Xml.h"

#include <cctype>
#include <cstdint>
#include <stdexcept>

#include "Text.h"

namespace nofences::xml {

namespace {

bool isNameChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.' || c == ':';
}

void appendUtf8(std::string& out, std::uint32_t codePoint) {
    if (codePoint < 0x80) {
        out += static_cast<char>(codePoint);
    } else if (codePoint < 0x800) {
        out += static_cast<char>(0xC0 | (codePoint >> 6));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else if (codePoint < 0x10000) {
        out += static_cast<char>(0xE0 | (codePoint >> 12));
        out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (codePoint >> 18));
        out += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codePoint & 0x3F));
    }
}

// Turns the entity or character reference at text[pos] (just after the '&') into its text, and moves pos past the ';'.
std::string decodeReference(const std::string& text, std::size_t& pos) {
    const auto end = text.find(';', pos);
    if (end == std::string::npos) {
        throw std::runtime_error("An XML reference isn't closed.");
    }
    const std::string name = text.substr(pos, end - pos);
    pos = end + 1;
    if (name == "lt") return "<";
    if (name == "gt") return ">";
    if (name == "amp") return "&";
    if (name == "quot") return "\"";
    if (name == "apos") return "'";
    if (!name.empty() && name[0] == '#') {
        const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
        const std::uint32_t codePoint = static_cast<std::uint32_t>(std::stoul(name.substr(hex ? 2 : 1), nullptr, hex ? 16 : 10));
        std::string utf8;
        appendUtf8(utf8, codePoint);
        return utf8;
    }
    throw std::runtime_error("Unknown XML entity &" + name + ";.");
}

class Parser {
public:
    explicit Parser(const std::string& document) : text_(document) {
        if (startsWith(text_, "\xEF\xBB\xBF")) {
            pos_ = 3;
        }
    }

    Element parseDocument() {
        skipProlog();
        if (pos_ >= text_.size() || text_[pos_] != '<') {
            throw std::runtime_error("The XML document has no root element.");
        }
        return parseElement();
    }

private:
    bool at(const std::string& literal) const { return text_.compare(pos_, literal.size(), literal) == 0; }

    void skipPast(const std::string& terminator) {
        const auto end = text_.find(terminator, pos_);
        if (end == std::string::npos) {
            throw std::runtime_error("The XML document ends inside a " + terminator + " section.");
        }
        pos_ = end + terminator.size();
    }

    void skipWhitespace() {
        while (pos_ < text_.size() && std::isspace(static_cast<unsigned char>(text_[pos_]))) {
            ++pos_;
        }
    }

    // Declarations, comments and doctypes come before the root element, and can sit between comments and whitespace.
    void skipProlog() {
        for (;;) {
            skipWhitespace();
            if (at("<?")) {
                skipPast("?>");
            } else if (at("<!--")) {
                skipPast("-->");
            } else if (at("<!")) {
                skipPast(">");
            } else {
                return;
            }
        }
    }

    std::string readName() {
        const std::size_t start = pos_;
        while (pos_ < text_.size() && isNameChar(text_[pos_])) {
            ++pos_;
        }
        if (pos_ == start) {
            throw std::runtime_error("An XML element has no name.");
        }
        return text_.substr(start, pos_ - start);
    }

    // Attributes are skipped, since the fence files don't use them.
    void skipAttributes() {
        char quote = 0;
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (quote) {
                if (c == quote) quote = 0;
            } else if (c == '"' || c == '\'') {
                quote = c;
            } else if (c == '>' || (c == '/' && pos_ + 1 < text_.size() && text_[pos_ + 1] == '>')) {
                return;
            }
            ++pos_;
        }
        throw std::runtime_error("An XML tag isn't closed.");
    }

    Element parseElement() {
        ++pos_;  // the '<'
        Element element;
        element.name = readName();
        skipAttributes();
        if (at("/>")) {
            pos_ += 2;
            return element;
        }
        ++pos_;  // the '>'

        for (;;) {
            if (pos_ >= text_.size()) {
                throw std::runtime_error("The XML element " + element.name + " isn't closed.");
            }
            if (at("</")) {
                pos_ += 2;
                const std::string closing = readName();
                if (closing != element.name) {
                    throw std::runtime_error("The XML element " + element.name + " is closed by " + closing + ".");
                }
                skipWhitespace();
                if (pos_ >= text_.size() || text_[pos_] != '>') {
                    throw std::runtime_error("The XML closing tag " + closing + " is malformed.");
                }
                ++pos_;
                return element;
            }
            if (at("<!--")) {
                skipPast("-->");
            } else if (text_[pos_] == '<') {
                element.children.push_back(parseElement());
            } else if (text_[pos_] == '&') {
                ++pos_;
                element.text += decodeReference(text_, pos_);
            } else {
                element.text += text_[pos_++];
            }
        }
    }

    const std::string& text_;
    std::size_t pos_ = 0;
};

std::string escape(const std::string& text) {
    std::string escaped;
    for (const char c : text) {
        switch (c) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            case '"': escaped += "&quot;"; break;
            case '\'': escaped += "&apos;"; break;
            default: escaped += c;
        }
    }
    return escaped;
}

void writeElement(std::string& out, const Element& element, int depth) {
    const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
    out += indent + "<" + element.name;
    if (element.children.empty() && element.text.empty()) {
        out += " />\n";
    } else if (element.children.empty()) {
        out += ">" + escape(element.text) + "</" + element.name + ">\n";
    } else {
        out += ">\n";
        for (const Element& child : element.children) {
            writeElement(out, child, depth + 1);
        }
        out += indent + "</" + element.name + ">\n";
    }
}

}  // namespace

const Element* Element::child(const std::string& childName) const {
    for (const Element& candidate : children) {
        if (candidate.name == childName) {
            return &candidate;
        }
    }
    return nullptr;
}

Element parse(const std::string& document) {
    return Parser(document).parseDocument();
}

std::string write(const Element& root) {
    std::string out = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n";
    writeElement(out, root, 0);
    return out;
}

}  // namespace nofences::xml
