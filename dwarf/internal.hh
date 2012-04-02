#ifndef _DWARFPP_INTERNAL_HH_
#define _DWARFPP_INTERNAL_HH_

#include "dwarf++.hh"

#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <vector>

DWARFPP_BEGIN_NAMESPACE

template<typename T>
std::string
to_hex(T v)
{
        static_assert(std::is_integral<T>::value,
                "to_hex applied to non-integral type");
        if (v == 0)
                return std::string("0");
        char buf[sizeof(T)*2 + 1];
        char *pos = &buf[sizeof(buf)-1];
        *pos-- = '\0';
        while (v && pos >= buf) {
                int digit = v & 0xf;
                if (digit < 10)
                        *pos = '0' + digit;
                else
                        *pos = 'a' + (digit - 10);
                pos--;
                v >>= 4;
        }
        return std::string(pos + 1);
}

enum class format
{
        unknown,
        dwarf32,
        dwarf64
};

/**
 * A single DWARF section or a slice of a section.  This also tracks
 * dynamic information necessary to decode values in this section.
 */
struct section
{
        section_type type;
        const char *begin, *end;
        const format fmt;
        unsigned addr_size;

        section(section_type type, const void *begin,
                section_length length, format fmt = format::unknown,
                unsigned addr_size = 0)
                : type(type), begin((char*)begin), end((char*)begin + length),
                  fmt(fmt), addr_size(addr_size) { }

        section(const section &o) = default;

        std::shared_ptr<section> slice(section_offset start, section_length len,
                                       format fmt = format::unknown,
                                       unsigned addr_size = 0)
        {
                if (fmt == format::unknown)
                        fmt = this->fmt;
                if (addr_size == 0)
                        addr_size = this->addr_size;

                return std::make_shared<section>(
                        type, begin+start,
                        std::min(len, (section_length)(end-begin)),
                        fmt, addr_size);
        }
};

/**
 * A cursor pointing into a DWARF section.  Provides deserialization
 * operations and bounds checking.
 */
struct cursor
{
        // XXX There's probably a lot of overhead to maintaining the
        // shared pointer to the section from this.  Perhaps the rule
        // should be that all objects keep the dwarf::impl alive
        // (directly or indirectly) and that keeps the loader alive,
        // so a cursor just needs a regular section*.

        std::shared_ptr<section> sec;
        const char *pos;

        cursor()
                : pos(nullptr) { }
        cursor(const std::shared_ptr<section> sec, section_offset offset = 0)
                : sec(sec), pos(sec->begin + offset) { }

        /**
         * Read a subsection.  The cursor must be at an initial
         * length.  After, the cursor will point just past the end of
         * the subsection.  The returned section has the appropriate
         * DWARF format and begins at the current location of the
         * cursor (so this is usually followed by a
         * skip_initial_length).
         */
        std::shared_ptr<section> subsection();
        std::int64_t sleb128();
        section_offset offset();
        void string(std::string &out);
        const char *cstr(size_t *size_out = nullptr);

        void
        ensure(section_offset bytes)
        {
                if ((section_offset)(sec->end - pos) < bytes || pos >= sec->end)
                        underflow();
        }

        template<typename T>
        T fixed()
        {
                ensure(sizeof(T));
                T val = *(T*)pos;
                pos += sizeof(T);
                return val;
        }

        std::uint64_t uleb128()
        {
                // Appendix C
                // XXX Pre-compute all two byte ULEB's
                std::uint64_t result = 0;
                int shift = 0;
                while (pos < sec->end) {
                        uint8_t byte = *(uint8_t*)(pos++);
                        result |= (uint64_t)(byte & 0x7f) << shift;
                        if ((byte & 0x80) == 0)
                                return result;
                        shift += 7;
                }
                underflow();
                return 0;
        }

        taddr address()
        {
                switch (sec->addr_size) {
                case 1:
                        return fixed<uint8_t>();
                case 2:
                        return fixed<uint16_t>();
                case 4:
                        return fixed<uint32_t>();
                case 8:
                        return fixed<uint64_t>();
                default:
                        throw std::runtime_error("address size " + std::to_string(sec->addr_size) + " not supported");
                }
        }

        void skip_initial_length();
        void skip_form(DW_FORM form);

        cursor &operator+=(section_offset offset)
        {
                pos += offset;
                return *this;
        }

        cursor operator+(section_offset offset) const
        {
                return cursor(sec, pos + offset);
        }

        bool operator<(const cursor &o) const
        {
                return pos < o.pos;
        }

        bool end() const
        {
                return pos >= sec->end;
        }

        bool valid() const
        {
                return !!pos;
        }

        section_offset get_section_offset() const
        {
                return pos - sec->begin;
        }

private:
        cursor(const std::shared_ptr<section> sec, const char *pos)
                : sec(sec), pos(pos) { }

        void underflow();
};

/**
 * An attribute specification in an abbrev.
 */
struct attribute_spec
{
        DW_AT name;
        DW_FORM form;

        // Computed information
        value::type type;

        attribute_spec(DW_AT name, DW_FORM form);
};

typedef std::uint64_t abbrev_code;

/**
 * An entry in .debug_abbrev.
 */
struct abbrev_entry
{
        abbrev_code code;
        DW_TAG tag;
        bool children;
        std::vector<attribute_spec> attributes;

        bool read(cursor *cur);
};

/**
 * A section header in .debug_info.
 */
struct info_unit
{
        // .debug_info-relative offset of this unit
        section_offset offset;
        uhalf version;
        // .debug_abbrev-relative offset of this unit's abbrevs
        section_offset debug_abbrev_offset;
        ubyte address_size;
        // Cursor to the first DIE in this unit.  This cursor's
        // section is limited to this unit.
        cursor entries;

        void read(cursor *cur)
        {
                offset = cur->get_section_offset();

                // Section 7.5.1.1
                std::shared_ptr<section> subsec = cur->subsection();
                cursor sub(subsec);
                sub.skip_initial_length();
                version = sub.fixed<uhalf>();
                if (version < 2 || version > 4)
                        throw format_error("unknown info unit version " + std::to_string(version));
                debug_abbrev_offset = sub.offset();
                address_size = sub.fixed<ubyte>();
                sub.sec->addr_size = address_size;
                entries = sub;
        }
};

/**
 * A section header in .debug_pubnames or .debug_pubtypes.
 */
struct name_unit
{
        uhalf version;
        section_offset debug_info_offset;
        section_length debug_info_length;
        // Cursor to the first name_entry in this unit.  This cursor's
        // section is limited to this unit.
        cursor entries;

        void read(cursor *cur)
        {
                // Section 7.19
                std::shared_ptr<section> subsec = cur->subsection();
                cursor sub(subsec);
                sub.skip_initial_length();
                version = sub.fixed<uhalf>();
                if (version != 2)
                        throw format_error("unknown name unit version " + std::to_string(version));
                debug_info_offset = sub.offset();
                debug_info_length = sub.offset();
                entries = sub;
        }
};

/**
 * An entry in a .debug_pubnames or .debug_pubtypes unit.
 */
struct name_entry
{
        section_offset offset;
        std::string name;

        void read(cursor *cur)
        {
                offset = cur->offset();
                cur->string(name);
        }
};

/**
 * Implementation of a DWARF file.
 */
struct dwarf::impl
{
        impl(const std::shared_ptr<loader> &l)
                : l(l) { }

        std::shared_ptr<loader> l;

        std::shared_ptr<section> sec_info;
        std::shared_ptr<section> sec_abbrev;

        std::vector<compilation_unit> compilation_units;

        const std::shared_ptr<section> get_section(section_type type)
        {
                if (type == section_type::info)
                        return sec_info;
                if (type == section_type::abbrev)
                        return sec_abbrev;

                auto it = sections.find(type);
                if (it != sections.end())
                        return it->second;

                size_t size;
                const void *data = l->load(type, &size);
                if (!data)
                        throw format_error(std::string(elf::section_type_to_name(type)) + " section missing");
                sections[type] = std::make_shared<section>(section_type::str, data, size);
                return sections[type];
        }

private:
        std::map<section_type, std::shared_ptr<section> > sections;
};

/**
 * Implementation of a compilation unit.
 */
struct compilation_unit::impl
{
        const std::shared_ptr<dwarf::impl> file;
        const std::shared_ptr<section> subsec;
        info_unit info;
        die root;

        // Map from abbrev code to abbrev
        std::unordered_map<abbrev_code, abbrev_entry> abbrevs;

        impl(std::shared_ptr<dwarf::impl> file, info_unit info)
                : file(file), subsec(info.entries.sec), info(info) { }

        void force_abbrevs();
};

DWARFPP_END_NAMESPACE

#endif
