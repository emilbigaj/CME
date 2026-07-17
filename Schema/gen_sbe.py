#!/usr/bin/env python3
"""
CME SBE code generator (iLink 3 order entry + MDP 3.0 market data).

Parses an official CME SBE schema XML and emits a single C++23 header of
#pragma pack(1) POD structs you cast straight over the wire, each with glaze reflection
and a ToString() -- the same house style as the HFT lib (Order.hpp / Instrument.hpp).

Profiles hold the schema-specific bits (namespace, expected package/id, framing):
    ilink3 : ilinkbinary.xml          -> namespace ILink3, SOFH framing (0xCAFE)
    mdp3   : templates_FixBinary.xml  -> namespace Mdp3, UDP packet framing

Design notes:
  * presence="constant" fields are NOT on the wire -> skipped from the struct layout,
    emitted as `static constexpr` instead. Getting this wrong shifts every later offset;
    the generated `static_assert(sizeof(T) == BlockLength)` is the backstop.
  * char[N] (N>1) -> Tools::StringN<N> (POD, trims, glaze-serializable) reused from HFT.
  * enums -> `enum class : underlying`; JSON names come from HFT's magic_enum glaze meta.
  * composites (PRICE9 etc.) -> struct with a Value() decimal helper.
  * PHASE 1: only the fixed root block. Repeating <group> and var-length <data> are NOT
    struct members (they start at BlockLength); they get cursor/decoder helpers in phase 2.

Usage: gen_sbe.py <profile> <schema.xml> <out.hpp>
"""
import sys
import xml.etree.ElementTree as ET

PROFILES = {
    'ilink3': dict(
        namespace='ILink3',
        title='CME iLink 3 SBE',
        expect_package='iLinkBinary',
        expect_id='8',
        framing='sofh',
    ),
    'mdp3': dict(
        namespace='Mdp3',
        title='CME MDP 3.0 SBE',
        expect_package='mktdata',
        expect_id='1',
        framing='packet',
    ),
}

PRIM_CPP = {
    'char': 'char', 'int8': 'int8_t', 'uint8': 'uint8_t',
    'int16': 'int16_t', 'uint16': 'uint16_t', 'int32': 'int32_t',
    'uint32': 'uint32_t', 'int64': 'int64_t', 'uint64': 'uint64_t',
    'float': 'float', 'double': 'double',
}
PRIM_SIZE = {
    'char': 1, 'int8': 1, 'uint8': 1, 'int16': 2, 'uint16': 2,
    'int32': 4, 'uint32': 4, 'int64': 8, 'uint64': 8,
    'float': 4, 'double': 8,
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
    def __init__(self, path, namespace):
        self.ns = namespace
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
        self.package = self.root.get('package')
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
            return f'{self.ns}::{ident(ftype)}'
        if ftype in self.sets:
            return f'{self.ns}::{ident(ftype)}'
        if ftype in self.composites:
            return f'{self.ns}::{ident(ftype)}'
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

    def composite_members(self, comp):
        """Composite members as <type>-like elements; <ref> members resolve to their target."""
        out = []
        for m in comp:
            t = tag(m)
            if t == 'type':
                out.append(m)
            elif t == 'ref':
                target = m.get('type')
                if target in self.types:
                    out.append(self.types[target])
                else:
                    raise SystemExit(f'composite {comp.get("name")}: unsupported <ref type="{target}">')
        return out

    def composite_size(self, comp):
        total = 0
        for m in self.composite_members(comp):
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
    for m in s.composite_members(comp):
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


def collect_layout(s, container):
    """Read one message or group element's direct children into: the wire fields (sorted by
    offset), the constant fields (not on the wire), and any child <group>/<data> elements."""
    layout = []       # (offset, size, cppType, memberName)
    constants = []    # (cppType, memberName, value)
    child_groups = [] # nested <group> elements
    child_data = []   # nested <data> elements
    computed = 0      # running offset for fields with no explicit offset attribute
    for f in container:
        t = tag(f)
        if t == 'group':
            child_groups.append(f)
            continue
        if t == 'data':
            child_data.append(f)
            continue
        if t != 'field':
            continue
        ftype = f.get('type')
        if s.is_constant_field(ftype):
            tdef = s.types[ftype]
            constants.append((s.field_cpp_type(ftype), ident(f.get('name')), (tdef.text or '').strip()))
            continue
        size = s.field_wire_size(ftype)
        off_attr = f.get('offset')
        off = int(off_attr) if off_attr is not None else computed
        computed = off + size
        layout.append((off, size, s.field_cpp_type(ftype), ident(f.get('name'))))
    layout.sort(key=lambda x: x[0])
    return layout, constants, child_groups, child_data


def emit_fixed_block(name, block_length, layout, constants, header_lines, out):
    """Emit one #pragma pack(1) cast-over-the-wire struct for a fixed block (a message root
    block or a group entry): header constants, the fields (with padding for gaps), the
    constant fields, glaze/ToString, and the size checks. Returns the emitted field names."""
    out.append('#pragma pack(push, 1)')
    out.append(f'struct {name}')
    out.append('{')
    for line in header_lines:
        out.append('\t' + line)

    members = []
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
        if cpp.startswith('Tools::StringN'):
            out.append(f'\tstatic constexpr std::string_view {mname} = "{val}";')
        else:
            lit = val if cpp != 'char' else f"'{val}'"
            out.append(f'\tstatic constexpr {cpp} {mname} = {lit};')

    out.append('\tstd::string ToString() const { return Tools::Json::Serialize(*this); }')
    glaze_fields = ', '.join(f'"{m}", &T::{m}' for m in members)
    out.append('\tstruct glaze { using T = ' + name + ';')
    out.append(f'\t\tstatic constexpr auto value = glz::object({glaze_fields}); }};')
    out.append('};')
    out.append('#pragma pack(pop)')
    out.append(f'static_assert(Tools::PlainOldData<{name}>);')
    if block_length == 0 and not members:
        # An empty block (e.g. MDP3 AdminHeartbeat) is 1 byte in C++, so the wire check cannot
        # hold. Never cast or copy such a struct over bytes.
        out.append(f'static_assert(sizeof({name}) == 1, "{name}: empty block, do not cast over the wire");')
    else:
        out.append(f'static_assert(sizeof({name}) == {block_length}, "{name} block size mismatch");')
    out.append('')
    return members


def emit_group_entry(s, message_name, group, out):
    """Emit the per-entry struct for one repeating group, named <Message>_<Group>. Returns
    (groupName, entryStructName, entryBlockLength). Nested groups/data inside an entry are not
    modelled yet -- flagged in a comment."""
    group_name = ident(group.get('name'))
    entry_name = f'{message_name}_{group_name}'
    block_length = int(group.get('blockLength'))
    layout, constants, nested_groups, nested_data = collect_layout(s, group)

    out.append(f'// group {group.get("name")} of {message_name}  (entry blockLength {block_length})')
    if nested_groups or nested_data:
        skipped = ', '.join(x.get('name') for x in nested_groups + nested_data)
        out.append(f'// NOTE: nested {skipped} inside this entry is not modelled yet')
    header = [f'static constexpr uint16_t BlockLength = {block_length};']
    emit_fixed_block(entry_name, block_length, layout, constants, header, out)
    return group_name, entry_name, block_length


def emit_message_reader(message_name, group_infos, out):
    """Emit a reader that walks a message's repeating groups in order. Groups are laid out one
    after another past the root block, so each group starts where the previous one ended."""
    reader = message_name + 'Groups'
    out.append(f'// Walks the repeating groups of a {message_name} in wire order.')
    out.append(f'struct {reader}')
    out.append('{')
    out.append('\tconst uint8_t* Root;   // start of the message body (the root block)')
    out.append(f'\tstatic {reader} Of(const {message_name}& message) {{ return {{reinterpret_cast<const uint8_t*>(&message)}}; }}')
    prev = None
    for group_name, entry_name, _ in group_infos:
        start = f'Root + {message_name}::BlockLength' if prev is None else f'{prev}().End()'
        out.append(f'\tSbe::GroupReader<{entry_name}> {group_name}() const {{ return Sbe::GroupReader<{entry_name}>({start}); }}')
        prev = group_name
    out.append('};')
    out.append('')


def emit_to_json_with_groups(message_name, group_infos, out):
    """Emit a helper that renders a message's root block plus each of its repeating groups as one
    JSON line, so the log shows the group entries (party details, book levels, and the like) and
    not only the fixed root. Renders the message's top-level groups; entries with their own
    nested groups (not modelled) show their fixed fields only."""
    out.append(f'// {message_name}: root block plus its repeating groups, as one JSON line.')
    out.append(f'inline std::string ToJsonLine_{message_name}(const void* body)')
    out.append('{')
    out.append(f'\tconst {message_name}& message = *reinterpret_cast<const {message_name}*>(body);')
    out.append('\tstd::string line = Tools::Json::SerializeToLine(message);')
    out.append("\tif (!line.empty() && line.back() == '}')")
    out.append('\t\tline.pop_back();   // reopen the root object to append the groups')
    out.append("\tbool needComma = !line.empty() && line.back() != '{';")
    out.append(f'\tauto groups = {message_name}Groups::Of(message);')
    for group_name, _entry_name, _ in group_infos:
        out.append(f'\tline += needComma ? ",\\"{group_name}\\":[" : "\\"{group_name}\\":[";')
        out.append('\tneedComma = true;')
        out.append('\t{')
        out.append('\t\tbool first = true;')
        out.append(f'\t\tfor (const auto& entry : groups.{group_name}())')
        out.append('\t\t{')
        out.append("\t\t\tif (!first) line += ',';")
        out.append('\t\t\tfirst = false;')
        out.append('\t\t\tline += Tools::Json::SerializeToLine(entry);')
        out.append('\t\t}')
        out.append('\t}')
        out.append("\tline += ']';")
    out.append("\tline += '}';")
    out.append('\treturn line;')
    out.append('}')
    out.append('')


def emit_message(s, msg, out):
    mid = msg.get('id')
    raw_name = msg.get('name')
    name = ident(clean_message_name(raw_name, mid))
    block_length = int(msg.get('blockLength'))
    semantic = msg.get('semanticType', '')
    layout, constants, child_groups, child_data = collect_layout(s, msg)

    # Step 1: The message root block.
    out.append(f'// {raw_name}  (template {mid}, blockLength {block_length}, semanticType "{semantic}")')
    if child_data:
        out.append(f'// trailing variable-length data (not modelled): {", ".join(d.get("name") for d in child_data)}')
    header = [
        f'static constexpr uint16_t TemplateId = {mid};',
        f'static constexpr uint16_t BlockLength = {block_length};',
        f'static constexpr std::string_view ObjectType = "{raw_name}";',
    ]
    if semantic:
        header.append(f'static constexpr std::string_view SemanticType = "{semantic}";')
    emit_fixed_block(name, block_length, layout, constants, header, out)

    # Step 2: A per-entry struct for each repeating group, then a reader to walk them.
    group_infos = [emit_group_entry(s, name, g, out) for g in child_groups]
    if group_infos:
        emit_message_reader(name, group_infos, out)

    return name, mid, raw_name, group_infos


def emit_framing(cfg, out):
    out.append('// SBE message header (precedes every message body).')
    out.append('#pragma pack(push, 1)')
    out.append('struct MessageHeader { uint16_t BlockLength; uint16_t TemplateId; uint16_t SchemaId; uint16_t Version; };')
    out.append('static_assert(sizeof(MessageHeader) == 8);')
    if cfg['framing'] == 'sofh':
        out.append('// Simple Open Framing Header: EncodingType 0xCAFE = CME SBE 1.0 little-endian.')
        out.append('struct SimpleOpenFramingHeader { uint16_t MessageLength; uint16_t EncodingType; };')
        out.append('static constexpr uint16_t SbeEncodingType = 0xCAFE;')
        out.append('static_assert(sizeof(SimpleOpenFramingHeader) == 4);')
    elif cfg['framing'] == 'packet':
        out.append('// MDP3 UDP packet: [PacketHeader][ MessageSize | MessageHeader | body ]*  repeated to end of datagram.')
        out.append('struct PacketHeader { uint32_t MsgSeqNum; uint64_t SendingTime; };')
        out.append('static_assert(sizeof(PacketHeader) == 12);')
        out.append('// MessageSize counts itself + MessageHeader + body (i.e. offset to the next message).')
        out.append('struct MessageSize { uint16_t Size; };')
        out.append('static_assert(sizeof(MessageSize) == 2);')
    out.append('#pragma pack(pop)')
    out.append('')


def main():
    if len(sys.argv) != 4 or sys.argv[1] not in PROFILES:
        sys.exit(f'usage: gen_sbe.py <{"|".join(PROFILES)}> <schema.xml> <out.hpp>')
    cfg = PROFILES[sys.argv[1]]
    s = Schema(sys.argv[2], cfg['namespace'])

    if s.package != cfg['expect_package'] or s.id != cfg['expect_id']:
        sys.exit(f'schema mismatch: got package="{s.package}" id={s.id}, '
                 f'expected package="{cfg["expect_package"]}" id={cfg["expect_id"]}')

    out = []
    out.append('#pragma once')
    out.append('// AUTO-GENERATED by Schema/gen_sbe.py -- DO NOT EDIT BY HAND.')
    out.append(f'// {cfg["title"]}  schema id {s.id}, version {s.version}, description {s.description}.')
    out.append('// Cast these #pragma pack(1) structs straight over the wire (little-endian).')
    out.append('')
    out.append('#include <cstdint>')
    out.append('#include <string>')
    out.append('#include <string_view>')
    out.append('#include "String.hpp"   // Tools::StringN')
    out.append('#include "Json.hpp"     // Tools::Json + enum glaze meta (magic_enum)')
    out.append('#include "Tools.hpp"    // Tools::PlainOldData')
    out.append('#include "SbeGroup.hpp" // Sbe::GroupReader / GroupSize (repeating groups)')
    out.append('')
    out.append(f'namespace {cfg["namespace"]}')
    out.append('{')
    out.append('')
    out.append(f'static constexpr uint16_t SchemaId = {s.id};')
    out.append(f'static constexpr uint16_t SchemaVersion = {s.version};')
    out.append('')
    emit_framing(cfg, out)

    out.append('// ===== Enumerations =====')
    for name in s.enums:
        emit_enum(s, name, out)
    out.append('// ===== Sets (bitfields) =====')
    for name in s.sets:
        emit_set(s, name, out)
    out.append('// ===== Composites =====')
    for name in s.composites:
        if name in ('messageHeader', 'groupSize', 'groupSizeEncoding', 'groupSize8Byte', 'DATA'):
            continue   # framing/dimension/var-data composites: explicit or phase 2
        emit_composite(s, name, out)

    out.append('// ===== Messages (fixed root block; repeating groups walked for logging) =====')
    templates = []
    for msg in descendants(s.root, 'message'):
        nm, mid, raw, ginfos = emit_message(s, msg, out)
        templates.append((nm, mid, raw, ginfos))

    out.append('// Template id -> message, for RX dispatch.')
    out.append('enum class Template : uint16_t')
    out.append('{')
    for nm, mid, _, _g in sorted(templates, key=lambda x: int(x[1])):
        out.append(f'\t{nm} = {mid},')
    out.append('};')
    out.append('')

    # Template id -> "<Name><id>" object type string, for logging a received message by its
    # header alone (no need to cast to the struct first). Unknown ids fall through to a stub.
    out.append('// Template id -> object-type name (e.g. 514 -> "NewOrderSingle514"), for logging.')
    out.append('inline std::string_view ToObjectType(uint16_t templateId)')
    out.append('{')
    out.append('\tswitch (templateId)')
    out.append('\t{')
    for _, mid, raw, _g in sorted(templates, key=lambda x: int(x[1])):
        out.append(f'\t\tcase {mid}: return "{raw}";')
    out.append('\t\tdefault: return "Unknown";')
    out.append('\t}')
    out.append('}')
    out.append('')

    # For every message with repeating groups, a helper that renders root block + groups on one
    # line, so the log shows group contents (party details, book levels) not just the root.
    for nm, _mid, _raw, ginfos in sorted(templates, key=lambda x: int(x[1])):
        if ginfos:
            emit_to_json_with_groups(nm, ginfos, out)

    # Template id + body pointer -> the message serialized as one compact JSON line. Cold
    # path only (used by the logger's drain thread). Messages carrying binary blobs may not
    # render as valid text, so callers should guard the call. Messages with groups render root
    # block + groups; flat messages render the root struct directly.
    out.append('// Template id + body -> the message as one compact JSON line, for logging.')
    out.append('inline std::string ToJsonLine(uint16_t templateId, const void* body)')
    out.append('{')
    out.append('\tswitch (templateId)')
    out.append('\t{')
    for nm, mid, _, ginfos in sorted(templates, key=lambda x: int(x[1])):
        if ginfos:
            out.append(f'\t\tcase {mid}: return ToJsonLine_{nm}(body);')
        else:
            out.append(f'\t\tcase {mid}: return Tools::Json::SerializeToLine(*reinterpret_cast<const {nm}*>(body));')
    out.append('\t\tdefault: return "{}";')
    out.append('\t}')
    out.append('}')
    out.append('')
    out.append(f'}} // namespace {cfg["namespace"]}')
    out.append('')

    with open(sys.argv[3], 'w') as fh:
        fh.write('\n'.join(out))
    print(f'generated {sys.argv[3]}: {len(s.enums)} enums, {len(s.sets)} sets, '
          f'{len(s.composites)} composites, {len(templates)} messages')


if __name__ == '__main__':
    main()
