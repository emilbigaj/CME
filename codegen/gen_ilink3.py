#!/usr/bin/env python3
"""
CME iLink 3 SBE code generator.

Parses the official ilinkbinary.xml (schema id 8) and emits a single C++23 header of
#pragma pack(1) POD structs you cast straight over the wire, each with glaze reflection
and a ToString() -- the same house style as the HFT lib (Order.hpp / Instrument.hpp).

Design notes:
  * presence="constant" fields are NOT on the wire -> skipped from the struct layout,
    emitted as `static constexpr` instead. Getting this wrong shifts every later offset;
    the generated `static_assert(sizeof(T) == BlockLength)` is the backstop.
  * char[N] (N>1) -> Tools::StringN<N> (POD, trims, glaze-serializable) reused from HFT.
  * enums -> `enum class : underlying`; JSON names come from HFT's magic_enum glaze meta.
  * composites (PRICE9 etc.) -> struct with a Value() decimal helper.
  * PHASE 1: only the fixed root block. Repeating <group> and var-length <data> are NOT
    struct members (they start at BlockLength); they get cursor/decoder helpers in phase 2.

Usage: gen_ilink3.py <ilinkbinary.xml> <out.hpp>
"""
import sys
import xml.etree.ElementTree as ET

NS = '{http://www.fixprotocol.org/ns/simple/1.0}'

PRIM_CPP = {
    'char': 'char', 'int8': 'int8_t', 'uint8': 'uint8_t',
    'int16': 'int16_t', 'uint16': 'uint16_t', 'int32': 'int32_t',
    'uint32': 'uint32_t', 'int64': 'int64_t', 'uint64': 'uint64_t',
}
PRIM_SIZE = {
    'char': 1, 'int8': 1, 'uint8': 1, 'int16': 2, 'uint16': 2,
    'int32': 4, 'uint32': 4, 'int64': 8, 'uint64': 8,
}
CPP_KEYWORDS = {
    'new', 'delete', 'default', 'operator', 'this', 'class', 'struct',
    'template', 'typename', 'namespace', 'public', 'private', 'protected',
    'int', 'char', 'float', 'double', 'short', 'long', 'unsigned', 'signed',
    'switch', 'case', 'return', 'void', 'bool', 'const', 'auto', 'register',
}

def tag(el):
    """Local element name, ignoring any XML namespace prefix."""
    return el.tag.split('}', 1)[-1]

def children(el, localname):
    return [c for c in el if tag(c) == localname]

def descendants(root, localname):
    return [e for e in root.iter() if tag(e) == localname]

def ident(name):
    """Sanitize an XML name/value into a legal C++ identifier."""
    out = ''.join(c if (c.isalnum() or c == '_') else '_' for c in name)
    if not out or out[0].isdigit():
        out = '_' + out
    if out in CPP_KEYWORDS:
        out += '_'
    return out


class Schema:
    def __init__(self, path):
        self.root = ET.parse(path).getroot()
        self.types = {}       # name -> <type> element
        self.composites = {}  # name -> <composite>
        self.enums = {}       # name -> <enum>
        self.sets = {}        # name -> <set>
        for types_el in descendants(self.root, 'types'):
            for child in types_el:            # DIRECT children only (skip composite-internal <type>)
                t = tag(child)
                name = child.get('name')
                if t == 'type':
                    self.types[name] = child
                elif t == 'composite':
                    self.composites[name] = child
                elif t == 'enum':
                    self.enums[name] = child
                elif t == 'set':
                    self.sets[name] = child
        self.id = self.root.get('id')
        self.version = self.root.get('version')
        self.description = self.root.get('description')

    # ---- resolution ----
    def underlying_prim(self, encoding_type):
        """Resolve an enum/set encodingType (a type name or primitive) to a primitiveType keyword."""
        if encoding_type in self.types:
            return self.types[encoding_type].get('primitiveType')
        if encoding_type in PRIM_SIZE:
            return encoding_type
        raise KeyError(f'unknown encodingType {encoding_type}')

    def is_constant_field(self, ftype):
        t = self.types.get(ftype)
        return t is not None and t.get('presence') == 'constant'

    def field_cpp_type(self, ftype):
        # Qualify user-defined types with the namespace: CME names several fields the same
        # as their enum type (SplitMsg SplitMsg;), which unqualified trips -Wchanges-meaning.
        if ftype in self.enums:
            return f'ILink3::{ident(ftype)}'
        if ftype in self.sets:
            return f'ILink3::{ident(ftype)}'
        if ftype in self.composites:
            return f'ILink3::{ident(ftype)}'
        t = self.types[ftype]
        prim = t.get('primitiveType')
        length = t.get('length')
        if prim == 'char' and length and int(length) > 1:
            return f'Tools::StringN<{length}>'
        return PRIM_CPP[prim]

    def field_wire_size(self, ftype):
        if ftype in self.enums:
            return PRIM_SIZE[self.underlying_prim(self.enums[ftype].get('encodingType'))]
        if ftype in self.sets:
            return PRIM_SIZE[self.underlying_prim(self.sets[ftype].get('encodingType'))]
        if ftype in self.composites:
            return self.composite_size(self.composites[ftype])
        t = self.types[ftype]
        prim = t.get('primitiveType')
        length = t.get('length')
        if prim == 'char' and length:
            return int(length)
        return PRIM_SIZE[prim]

    def composite_size(self, comp):
        total = 0
        for m in comp:
            if tag(m) != 'type':
                continue
            if m.get('presence') == 'constant':   # constant sub-fields are not on the wire
                continue
            prim = m.get('primitiveType')
            length = m.get('length')
            total += int(length) if (prim == 'char' and length) else PRIM_SIZE[prim]
        return total


# ---------------------------------------------------------------- emitters
def emit_enum(s, name, out):
    el = s.enums[name]
    prim = s.underlying_prim(el.get('encodingType'))
    cpp = PRIM_CPP[prim]
    is_char = (prim == 'char')
    out.append(f'enum class {ident(name)} : {cpp}')
    out.append('{')
    for vv in children(el, 'validValue'):
        vname = ident(vv.get('name'))
        raw = (vv.text or '').strip()
        if is_char:
            esc = raw.replace('\\', '\\\\').replace("'", "\\'")
            out.append(f"\t{vname} = '{esc}',")
        else:
            out.append(f'\t{vname} = {raw},')
    out.append('};')
    out.append('')


def emit_set(s, name, out):
    el = s.sets[name]
    prim = s.underlying_prim(el.get('encodingType'))
    cpp = PRIM_CPP[prim]
    n = ident(name)
    out.append(f'struct {n}')
    out.append('{')
    out.append(f'\t{cpp} Value;')
    for ch in children(el, 'choice'):
        cname = ident(ch.get('name'))
        bit = int((ch.text or '0').strip())
        out.append(f'\t[[nodiscard]] bool {cname}() const {{ return (Value & ({cpp}(1) << {bit})) != 0; }}')
    out.append('\tstd::string ToString() const { return Tools::Json::Serialize(*this); }')
    out.append('\tstruct glaze { using T = ' + n + ';')
    out.append(f'\t\tstatic constexpr auto value = glz::object("Value", &T::Value); }};')
    out.append('};')
    out.append(f'static_assert(Tools::PlainOldData<{n}>);')
    out.append(f'static_assert(sizeof({n}) == {PRIM_SIZE[prim]});')
    out.append('')


def emit_composite(s, name, out):
    comp = s.composites[name]
    n = ident(name)
    members = []          # (cppType, memberName)
    constants = []        # (cppType, memberName, value)
    scale_exp = None
    for m in comp:
        if tag(m) != 'type':
            continue
        raw = m.get('name')
        mname = ident(raw[0].upper() + raw[1:])   # PascalCase composite members (Mantissa, Year...)
        prim = m.get('primitiveType')
        cpp = PRIM_CPP[prim]
        if m.get('presence') == 'constant':
            val = (m.text or '').strip()
            constants.append((cpp, mname, val))
            if mname.lower() == 'exponent':
                scale_exp = int(val)
        else:
            members.append((cpp, mname))
    out.append('#pragma pack(push, 1)')   # else int64+int8 aligns to 16, not the 9-byte wire size
    out.append(f'struct {n}')
    out.append('{')
    for cpp, mname in members:
        out.append(f'\t{cpp} {mname};')
    for cpp, mname, val in constants:
        out.append(f'\tstatic constexpr {cpp} {mname} = {val};')
    # decimal helper: a single mantissa + constant exponent
    if scale_exp is not None and len(members) == 1 and members[0][1].lower() == 'mantissa':
        factor = f'1e{scale_exp}' if scale_exp < 0 else f'1e+{scale_exp}'
        out.append(f'\t[[nodiscard]] double Value() const {{ return static_cast<double>(Mantissa) * {factor}; }}')
    out.append('\tstd::string ToString() const { return Tools::Json::Serialize(*this); }')
    glaze_fields = ', '.join(f'"{mn}", &T::{mn}' for _, mn in members)
    out.append('\tstruct glaze { using T = ' + n + ';')
    out.append(f'\t\tstatic constexpr auto value = glz::object({glaze_fields}); }};')
    out.append('};')
    out.append('#pragma pack(pop)')
    out.append(f'static_assert(Tools::PlainOldData<{n}>);')
    out.append(f'static_assert(sizeof({n}) == {s.composite_size(comp)});')
    out.append('')


def clean_message_name(name, mid):
    return name[:-len(mid)] if name.endswith(mid) else name


def emit_message(s, msg, out):
    mid = msg.get('id')
    raw_name = msg.get('name')
    name = ident(clean_message_name(raw_name, mid))
    block_length = int(msg.get('blockLength'))
    semantic = msg.get('semanticType', '')

    layout = []     # (offset, size, cppType, memberName)  wire fields, ascending offset
    constants = []  # (cppType, memberName, value)
    groups = []     # group/data names deferred to phase 2
    for f in msg:
        t = tag(f)
        if t == 'group':
            groups.append('group ' + f.get('name'))
            continue
        if t == 'data':
            groups.append('data ' + f.get('name'))
            continue
        if t != 'field':
            continue
        ftype = f.get('type')
        if s.is_constant_field(ftype):
            tdef = s.types[ftype]
            constants.append((s.field_cpp_type(ftype), ident(f.get('name')), (tdef.text or '').strip()))
            continue
        off = int(f.get('offset'))
        layout.append((off, s.field_wire_size(ftype), s.field_cpp_type(ftype), ident(f.get('name'))))
    layout.sort(key=lambda x: x[0])

    out.append('#pragma pack(push, 1)')
    out.append(f'// {raw_name}  (template {mid}, blockLength {block_length}, semanticType "{semantic}")')
    if groups:
        out.append(f'// PHASE 2 trailing after root block: {", ".join(groups)}')
    out.append(f'struct {name}')
    out.append('{')
    out.append(f'\tstatic constexpr uint16_t TemplateId = {mid};')
    out.append(f'\tstatic constexpr uint16_t BlockLength = {block_length};')
    if semantic:
        out.append(f'\tstatic constexpr std::string_view SemanticType = "{semantic}";')

    members = []      # emitted instance member names for glaze (excl. padding)
    cursor = 0
    pad_ix = 0
    for off, size, cpp, mname in layout:
        if off > cursor:
            out.append(f'\tuint8_t _pad{pad_ix}[{off - cursor}] = {{}};')
            pad_ix += 1
            cursor = off
        elif off < cursor:
            raise SystemExit(f'{name}: field {mname} offset {off} overlaps cursor {cursor}')
        out.append(f'\t{cpp} {mname};')
        members.append(mname)
        cursor += size
    if cursor < block_length:
        out.append(f'\tuint8_t _pad{pad_ix}[{block_length - cursor}] = {{}};')
    for cpp, mname, val in constants:
        lit = val if cpp not in ('char',) else f"'{val}'"
        # constants are strings/chars/ints; wrap char-array constants as string literals
        if cpp.startswith('Tools::StringN'):
            out.append(f'\tstatic constexpr std::string_view {mname} = "{val}";')
        else:
            out.append(f'\tstatic constexpr {cpp} {mname} = {lit};')

    out.append('\tstd::string ToString() const { return Tools::Json::Serialize(*this); }')
    glaze_fields = ', '.join(f'"{m}", &T::{m}' for m in members)
    out.append('\tstruct glaze { using T = ' + name + ';')
    out.append(f'\t\tstatic constexpr auto value = glz::object({glaze_fields}); }};')
    out.append('};')
    out.append('#pragma pack(pop)')
    out.append(f'static_assert(Tools::PlainOldData<{name}>);')
    out.append(f'static_assert(sizeof({name}) == {name}::BlockLength, "{name} root block size mismatch");')
    out.append('')
    return name, mid


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: gen_ilink3.py <ilinkbinary.xml> <out.hpp>')
    s = Schema(sys.argv[1])
    out = []
    out.append('#pragma once')
    out.append('// AUTO-GENERATED by codegen/gen_ilink3.py from ilinkbinary.xml -- DO NOT EDIT BY HAND.')
    out.append(f'// CME iLink 3 SBE  schema id {s.id}, version {s.version}, description {s.description}.')
    out.append('// Cast these #pragma pack(1) structs straight over the wire (little-endian).')
    out.append('')
    out.append('#include <cstdint>')
    out.append('#include <string>')
    out.append('#include <string_view>')
    out.append('#include "String.hpp"   // Tools::StringN')
    out.append('#include "Json.hpp"     // Tools::Json + enum glaze meta (magic_enum)')
    out.append('#include "Tools.hpp"    // Tools::PlainOldData')
    out.append('')
    out.append('namespace ILink3')
    out.append('{')
    out.append('')
    out.append(f'static constexpr uint16_t SchemaId = {s.id};')
    out.append(f'static constexpr uint16_t SchemaVersion = {s.version};')
    out.append('')
    out.append('// SBE message header (precedes every message body).')
    out.append('#pragma pack(push, 1)')
    out.append('struct MessageHeader { uint16_t BlockLength; uint16_t TemplateId; uint16_t SchemaId; uint16_t Version; };')
    out.append('static_assert(sizeof(MessageHeader) == 8);')
    out.append('// Simple Open Framing Header: EncodingType 0xCAFE = CME SBE 1.0 little-endian.')
    out.append('struct SimpleOpenFramingHeader { uint16_t MessageLength; uint16_t EncodingType; };')
    out.append('static constexpr uint16_t SbeEncodingType = 0xCAFE;')
    out.append('static_assert(sizeof(SimpleOpenFramingHeader) == 4);')
    out.append('#pragma pack(pop)')
    out.append('')

    out.append('// ===== Enumerations =====')
    for name in s.enums:
        emit_enum(s, name, out)
    out.append('// ===== Sets (bitfields) =====')
    for name in s.sets:
        emit_set(s, name, out)
    out.append('// ===== Composites =====')
    for name in s.composites:
        if name in ('messageHeader', 'groupSize', 'groupSizeEncoding', 'DATA'):
            continue   # framing/dimension/var-data composites: explicit or phase 2
        emit_composite(s, name, out)

    out.append('// ===== Messages (fixed root block; groups/data are phase 2) =====')
    templates = []
    for msg in descendants(s.root, 'message'):
        nm, mid = emit_message(s, msg, out)
        templates.append((nm, mid))

    out.append('// Template id -> message, for RX dispatch.')
    out.append('enum class Template : uint16_t')
    out.append('{')
    for nm, mid in sorted(templates, key=lambda x: int(x[1])):
        out.append(f'\t{nm} = {mid},')
    out.append('};')
    out.append('')
    out.append('} // namespace ILink3')
    out.append('')

    with open(sys.argv[2], 'w') as fh:
        fh.write('\n'.join(out))
    print(f'generated {sys.argv[2]}: {len(s.enums)} enums, {len(s.sets)} sets, '
          f'{len(s.composites)} composites, {len(templates)} messages')


if __name__ == '__main__':
    main()
