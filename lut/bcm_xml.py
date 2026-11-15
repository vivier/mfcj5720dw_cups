# SPDX-License-Identifier: GPL-3.0-or-later
"""IccXML structure with explicit BCM extensions and lossless binary storage."""
import re
import struct
import xml.etree.ElementTree as ET

if __package__:
    from . import bcm_text as bcm
else:
    import bcm_text as bcm

NAMESPACE = 'urn:brother:bcm:1'
EXT = '{' + NAMESPACE + '}'
ET.register_namespace('bcm', NAMESPACE)
TYPE_NAMES = {'description': 'textDescriptionType', 'copyright': 'textType',
              **{kind: EXT + kind.title() + 'Type' for kind in [*bcm.SIZES, 'raw']}}
TAG_NAMES = {bcm.DESCRIPTION: 'profileDescriptionTag', bcm.COPYRIGHT: 'copyrightTag'}
HEADER_NAMES = {'version': 'ProfileVersion', 'created': 'CreationDateTime',
                'manufacturer': 'DeviceManufacturer', 'model': 'DeviceModel'}
INTEGER_FIELDS = {'cube': [], 'curves': ['ink_limit'],
                  'halftone': [*bcm.NOISE, 'head_mode'],
                  'correction': list(bcm.CORRECTION),
                  'density': ['lower_percent', 'upper_percent'], 'raw': []}


def field_name(name):
    return EXT + ''.join(word.title() for word in name.split('_'))


def number(value):
    bcm.require(re.fullmatch(r'-?(?:0x[0-9a-fA-F]+|[0-9]+)', value) is not None,
                'expected a decimal or hexadecimal integer')
    return int(value, 16 if '0x' in value else 10)


def signature(value):
    """IccXML signatures use four characters or eight hex digits, optionally h."""
    if re.fullmatch(r'[0-9a-fA-F]{8}h?', value):
        return int(value.removesuffix('h'), 16)
    return bcm.tag_number(value)


def attributes(element, required=(), optional=()):
    bcm.keys(dict(element.attrib), required, optional)


def children(element, required=(), optional=(), repeated=()):
    """Reject misspellings, duplicates and stray text instead of dropping data."""
    bcm.require(not (element.text or '').strip(), f'unexpected text in {element.tag}')
    result = {}
    for child in element:
        bcm.require(not (child.tail or '').strip(), f'unexpected text after {child.tag}')
        bcm.require(child.tag in (*required, *optional, *repeated),
                    f'unknown element: {child.tag}')
        if child.tag in repeated:
            result.setdefault(child.tag, []).append(child)
        else:
            bcm.require(child.tag not in result, f'duplicate element: {child.tag}')
            result[child.tag] = child
    bcm.require(set(required) <= result.keys(),
                'missing elements: ' + ', '.join(sorted(set(required) - result.keys())))
    return result


def leaf(element, required=(), optional=()):
    attributes(element, required, optional)
    bcm.require(not len(element), f'{element.tag} must not contain child elements')
    return element.text or ''


def add(parent, name, value=None, **attrs):
    element = ET.SubElement(parent, name, {key: str(item) for key, item in attrs.items()})
    if value is not None:
        element.text = str(value)
    return element


def xml_character(char):
    code = ord(char)
    return (code in (9, 10, 13) or 0x20 <= code <= 0xd7ff or
            0xe000 <= code <= 0xfffd or 0x10000 <= code <= 0x10ffff)


def add_text(parent, name, value, encoding='latin1', **attrs):
    """Use XML text normally; retain XML-forbidden bytes only as an exception."""
    if all(xml_character(char) for char in value):
        return add(parent, name, value, **attrs)
    if name == 'TextData':
        name = 'HexTextData'
    else:
        attrs[EXT + 'Encoding'] = 'hex'
    return add(parent, name, value.encode(encoding, errors='surrogatepass').hex(' '), **attrs)


def read_text(element, encoding='latin1', optional=()):
    value = leaf(element, optional=[*optional, EXT + 'Encoding'])
    mode = element.get(EXT + 'Encoding')
    bcm.require(mode in (None, 'hex'), 'unknown text encoding')
    if element.tag == 'HexTextData' or mode == 'hex':
        value = bcm.octets(value).decode(encoding, errors='surrogatepass')
    return value


def text_parts(data, encoding='latin1'):
    value = data.decode(encoding, errors='surrogatepass').split('\0', 1)[0]
    encoded = value.encode(encoding, errors='surrogatepass')
    padding = data[len(encoded):]
    normal = bcm.text_padding(encoded, 2 if encoding == 'utf-16-le' else 1)
    return value, None if padding == normal else padding.hex(' ')


def add_padded_text(parent, name, data, padding_name, encoding='latin1', **attrs):
    value, padding = text_parts(data, encoding)
    add_text(parent, name, value, encoding, **attrs)
    if padding is not None:
        add(parent, EXT + padding_name, padding)


def padded_text(fields, name, padding_name, encoding='latin1', optional=()):
    value = read_text(fields[name], encoding, optional)
    bcm.require('\0' not in value, f'{name} must not contain NUL; use {padding_name}')
    data = value.encode(encoding, errors='surrogatepass')
    width = 2 if encoding == 'utf-16-le' else 1
    pad = fields.get(EXT + padding_name)
    padding = bcm.octets(leaf(pad)) if pad is not None else bcm.text_padding(data, width)
    bcm.require(len(padding) % width == 0, 'padding must contain complete code units')
    return data + padding


def primary_text(fields):
    present = [name for name in ('TextData', 'HexTextData') if name in fields]
    bcm.require(len(present) == 1, 'expected exactly one TextData or HexTextData')
    return present[0]


def description_parts(block):
    """Recognize all three text copies, including Brother's short tag length."""
    declared = bcm.encode_block(block)
    data = declared + bcm.octets(block.get('after', ''))
    first_end = 12 + struct.unpack_from('>I', data, 8)[0]
    offset = first_end
    result = [data[12:first_end]]
    for width in (2, 1):
        if offset + 8 > len(data):
            return None
        marker, count = struct.unpack_from('>II', data, offset)
        end = offset + 8 + count * width
        if end > len(data):
            return None
        result.extend([marker, data[offset + 8:end]])
        offset = end
    return (*result, len(data) - len(declared)) if offset == len(data) else None


def write_description(parent, block):
    if any(bcm.octets(block['reserved'])):
        add(parent, EXT + 'Reserved', block['reserved'])
    parts = description_parts(block)
    if parts is not None:
        text, language, unicode, reserved, legacy, tail = parts
        add_padded_text(parent, 'TextData', text, 'TextPadding')
        attrs = {'LanguageCode': f'{language:08X}h'} if language else {}
        add_padded_text(parent, 'Unicode', unicode, 'UnicodePadding', 'utf-16-le', **attrs)
        attrs = {'Reserved': reserved} if reserved else {}
        add_padded_text(parent, EXT + 'LegacyText', legacy, 'LegacyPadding', **attrs)
        add(parent, EXT + 'UndeclaredTailBytes', tail)
    else:
        data = bcm.latin1(block['text']) + bcm.octets(block['padding'])
        add_padded_text(parent, 'TextData', data, 'TextPadding')
        add(parent, EXT + 'Trailing', block['trailing'])
        write_after(parent, block)


def read_description(element, block):
    attributes(element)
    fields = children(element, optional=['TextData', 'HexTextData', 'Unicode', EXT + 'LegacyText',
                      *[EXT + name for name in ('Reserved', 'TextPadding', 'UnicodePadding',
                        'LegacyPadding', 'UndeclaredTailBytes', 'Trailing', 'After')]])
    text = padded_text(fields, primary_text(fields), 'TextPadding')
    reserved = bcm.octets(leaf(fields[EXT + 'Reserved']), 4) if EXT + 'Reserved' in fields else b'\0' * 4
    data = b'desc' + reserved + struct.pack('>I', len(text)) + text
    after = bcm.octets(leaf(fields[EXT + 'After'])) if EXT + 'After' in fields else b''
    if EXT + 'Trailing' in fields:
        bcm.require(not set(fields) & {'Unicode', EXT + 'LegacyText', EXT + 'UnicodePadding',
                    EXT + 'LegacyPadding', EXT + 'UndeclaredTailBytes'}, 'mixed description layouts')
        data += bcm.octets(leaf(fields[EXT + 'Trailing']))
    else:
        bcm.require({'Unicode', EXT + 'LegacyText', EXT + 'UndeclaredTailBytes'} <= fields.keys(),
                    'description requires Unicode, LegacyText and UndeclaredTailBytes')
        unicode = padded_text(fields, 'Unicode', 'UnicodePadding', 'utf-16-le', ['LanguageCode'])
        language = signature(fields['Unicode'].get('LanguageCode', '00000000'))
        legacy = padded_text(fields, EXT + 'LegacyText', 'LegacyPadding', optional=['Reserved'])
        marker = bcm.integer(number(fields[EXT + 'LegacyText'].get('Reserved', '0')))
        data += struct.pack('>II', language, len(unicode) // 2) + unicode
        data += struct.pack('>II', marker, len(legacy)) + legacy
        tail = bcm.integer(number(leaf(fields[EXT + 'UndeclaredTailBytes']).strip()))
        bcm.require(tail <= len(data), 'undeclared tail exceeds description size')
        end = len(data) - tail
        after = data[end:] + after
        data = data[:end]
    block.update(bcm.decode_block('description', data))
    block['after'] = after.hex(' ')


def write_after(parent, block):
    if bcm.octets(block.get('after', '')):
        add(parent, EXT + 'After', block['after'])


def float_text(value):
    bits = bcm.float_bytes(value)
    for digits in range(1, 10):
        text = format(value, f'.{digits}g')
        try:
            if bcm.float_bytes(float(text)) == bits:
                return text if any(c in text for c in '.eE') else text + '.0'
        except bcm.FormatError:
            continue
    raise bcm.FormatError('cannot format float32')


def write_block(parent, block):
    kind = block['kind']
    if kind == 'description':
        write_description(parent, block)
        return
    if kind == 'copyright':
        add_padded_text(parent, 'TextData', bcm.encode_block(block)[8:], 'TextPadding')
    else:
        if kind in bcm.SIGNATURE_KINDS:
            add_text(parent, EXT + 'Signature', block['signature'])
        for name in INTEGER_FIELDS[kind]:
            add(parent, field_name(name), block[name])
        if kind == 'cube':
            parent.append(ET.Comment(' RGB grid indices: 0..16; B varies fastest. CMYK samples: uint16. '))
            add(parent, 'Channels', InputChannels=3, OutputChannels=4)
            table = add(add(parent, 'CLUT', GridGranularity=17), 'TableData')
            # TableData stays an ordinary IccXML numeric array. Comments label
            # each input coordinate without becoming part of the sample data.
            nodes = {tuple(bcm.color_components(rgb, 'RGB')): bcm.color_components(cmyk, 'CMYK')
                     for rgb, cmyk in block['nodes'].items()}
            table.text = '\n            '
            for index in range(4913):
                rgb = index // 289, index // 17 % 17, index % 17
                comment = ET.Comment(' ' + bcm.color_expression('RGB', rgb) + ' -> CMYK ')
                table.append(comment)
                comment.tail = ' ' + ' '.join(f'{value:4d}' for value in nodes[rgb])
                comment.tail += '\n            ' if index < 4912 else '\n          '
        elif kind == 'curves':
            for name in bcm.CURVES:
                values = block['curves'][name]
                text = '\n' + '\n'.join('        ' + ' '.join(f'{n:6d}' for n in values[i:i + 16])
                                          for i in range(0, 256, 16)) + '\n      '
                add(parent, EXT + 'Curve', text, Name=name)
        elif kind == 'halftone':
            for group in ('thresholds', 'drops'):
                container = add(parent, field_name(group))
                for index, values in enumerate(block[group]):
                    channel = add(container, EXT + 'Channel', Index=index)
                    for name in bcm.DROPS:
                        add(channel, field_name(name), values[name])
        elif kind == 'correction':
            factors = add(parent, EXT + 'Factors')
            for name in bcm.CHANNELS:
                value = block['factors'][name]
                if isinstance(value, dict):
                    add(factors, field_name(name), Bits=f'0x{value["bits"]:08x}')
                else:
                    add(factors, field_name(name), float_text(value))
        elif kind == 'raw':
            add(parent, 'HexData', block['data'])
    write_after(parent, block)


def read_block(element, kind, tag):
    block = {'name': f'{kind}_{tag:08x}', 'kind': kind}
    attributes(element)
    if kind == 'description':
        read_description(element, block)
        return block
    if kind == 'copyright':
        fields = children(element, optional=['TextData', 'HexTextData', EXT + 'TextPadding', EXT + 'After'])
        data = padded_text(fields, primary_text(fields), 'TextPadding')
        block.update(bcm.decode_block(kind, bcm.COPYRIGHT_HEADER + data))
    else:
        required = [field_name(name) for name in INTEGER_FIELDS[kind]]
        if kind in bcm.SIGNATURE_KINDS:
            required.append(EXT + 'Signature')
        required += {'cube': ['Channels', 'CLUT'], 'halftone': [EXT + 'Thresholds', EXT + 'Drops'],
                     'correction': [EXT + 'Factors'], 'raw': ['HexData']}.get(kind, [])
        repeated = [EXT + 'Curve'] if kind == 'curves' else []
        fields = children(element, required, [EXT + 'After'], repeated)
        if kind in bcm.SIGNATURE_KINDS:
            block['signature'] = read_text(fields[EXT + 'Signature'])
        for name in INTEGER_FIELDS[kind]:
            block[name] = number(leaf(fields[field_name(name)]).strip())
        if kind == 'cube':
            channels = fields['Channels']
            bcm.require(not leaf(channels, ['InputChannels', 'OutputChannels']).strip(), 'Channels must be empty')
            bcm.require(number(channels.get('InputChannels')) == 3 and
                        number(channels.get('OutputChannels')) == 4, 'expected three input and four output channels')
            clut = fields['CLUT']
            attributes(clut, ['GridGranularity'])
            bcm.require(number(clut.get('GridGranularity')) == 17, 'expected a 17 x 17 x 17 CLUT')
            data = children(clut, ['TableData'])['TableData']
            values = [bcm.integer(number(n), 16) for n in leaf(data).split()]
            bcm.require(len(values) == 19652, 'expected 4913 CMYK tuples in TableData')
            block['nodes'] = {bcm.color_expression('RGB', (i // 289, i // 17 % 17, i % 17)):
                              bcm.color_expression('CMYK', values[4 * i:4 * i + 4]) for i in range(4913)}
        elif kind == 'curves':
            block['curves'] = {}
            for curve in fields.get(EXT + 'Curve', []):
                values = leaf(curve, ['Name'])
                name = curve.get('Name')
                bcm.require(name in bcm.CURVES and name not in block['curves'], 'unknown or duplicate curve')
                block['curves'][name] = [number(n) for n in values.split()]
        elif kind == 'halftone':
            for group in ('thresholds', 'drops'):
                container = fields[field_name(group)]
                attributes(container)
                entries = children(container, repeated=[EXT + 'Channel']).get(EXT + 'Channel', [])
                values = {}
                for channel in entries:
                    attributes(channel, ['Index'])
                    index = number(channel.get('Index'))
                    bcm.require(0 <= index < 4 and index not in values, 'invalid or duplicate channel index')
                    items = children(channel, [field_name(name) for name in bcm.DROPS])
                    values[index] = {name: number(leaf(items[field_name(name)]).strip()) for name in bcm.DROPS}
                bcm.require(len(values) == 4, 'expected four channels')
                block[group] = [values[index] for index in range(4)]
        elif kind == 'correction':
            container = fields[EXT + 'Factors']
            attributes(container)
            factors = children(container, [field_name(name) for name in bcm.CHANNELS])
            block['factors'] = {}
            for name in bcm.CHANNELS:
                factor = factors[field_name(name)]
                value = leaf(factor, optional=['Bits']).strip()
                if 'Bits' in factor.attrib:
                    bcm.require(not value, 'float32 Bits cannot also have a numeric value')
                    block['factors'][name] = {'bits': number(factor.get('Bits'))}
                else:
                    block['factors'][name] = float(value)
        elif kind == 'raw':
            block['data'] = leaf(fields['HexData'])
    if EXT + 'After' in fields:
        block['after'] = leaf(fields[EXT + 'After'])
    return block


class ProfileTreeBuilder(ET.TreeBuilder):
    """Profiles are self-contained XML; no DTDs or external data references."""
    def __init__(self):
        super().__init__()
        self.depth = 0

    def start(self, tag, attrs):
        self.depth += 1
        bcm.require(self.depth <= 32, 'XML nesting exceeds 32 levels')
        return super().start(tag, attrs)

    def end(self, tag):
        self.depth -= 1
        return super().end(tag)

    def doctype(self, name, pubid, system):
        raise bcm.FormatError('DTD declarations are not supported')


def read_header(element):
    attributes(element)
    fields = children(element, HEADER_NAMES.values(), [EXT + 'DirectoryPadding'])
    header = {name: read_text(fields[tag]) for name, tag in HEADER_NAMES.items()}
    stamp = re.fullmatch(r'([0-9]{4,5})-([0-9]{2,5})-([0-9]{2,5})T'
                         r'([0-9]{2,5}):([0-9]{2,5}):([0-9]{2,5})', header['created'])
    bcm.require(stamp is not None, 'expected CreationDateTime as YYYY-MM-DDTHH:MM:SS')
    header['created'] = bcm.creation_text(tuple(bcm.integer(int(n), 16) for n in stamp.groups()))
    pad = fields.get(EXT + 'DirectoryPadding')
    return header, leaf(pad) if pad is not None else ''


def tag_id(element):
    known = {value: key for key, value in TAG_NAMES.items()}
    if element.tag == 'PrivateTag':
        attributes(element, ['TagSignature'], ['SameAs', 'SameAsSignature'])
        return signature(element.get('TagSignature'))
    bcm.require(element.tag in known, 'unknown tag element: ' + element.tag)
    attributes(element, optional=['SameAs', 'SameAsSignature'])
    return known[element.tag]


def parse_document(text):
    bcm.require(len(text.encode('utf-8')) <= bcm.MAX_BYTES, 'description exceeds 64 MiB')
    try:
        root = ET.fromstring(text, parser=ET.XMLParser(target=ProfileTreeBuilder()))
    except (ET.ParseError, UnicodeError) as error:
        raise bcm.FormatError(str(error)) from error
    bcm.require(root.tag == 'IccProfile', 'expected IccProfile root')
    attributes(root)
    fields = children(root, ['Header', 'Tags'])
    bcm.require([node.tag for node in root] == ['Header', 'Tags'], 'Header must precede Tags')
    header, padding = read_header(fields['Header'])
    document = {'format': bcm.FORMAT, 'header': header, 'directory_padding': padding,
                'tags': [], 'blocks': []}
    tags = fields['Tags']
    attributes(tags)
    bcm.require(len(tags) <= bcm.MAX_TAGS, 'too many tags')
    bcm.require(not (tags.text or '').strip(), 'unexpected text in Tags')
    names = {}
    types = {value: key for key, value in TYPE_NAMES.items()}
    for tag_element in tags:
        tag = tag_id(tag_element)
        bcm.require(tag not in names, 'duplicate tag signature')
        bcm.require(not (tag_element.tail or '').strip(), 'unexpected text after tag')
        if 'SameAs' in tag_element.attrib:
            bcm.require(not len(tag_element) and not (tag_element.text or '').strip(), 'SameAs tag must be empty')
            target_name = tag_element.get('SameAs')
            if target_name == 'PrivateTag':
                bcm.require('SameAsSignature' in tag_element.attrib, 'missing SameAsSignature')
                target = signature(tag_element.get('SameAsSignature'))
            else:
                bcm.require('SameAsSignature' not in tag_element.attrib, 'unexpected SameAsSignature')
                known = {value: key for key, value in TAG_NAMES.items()}
                bcm.require(target_name in known, 'unknown SameAs target')
                target = known[target_name]
            bcm.require(target in names, 'SameAs target must precede its reference')
            name = names[target]
        else:
            bcm.require('SameAsSignature' not in tag_element.attrib, 'SameAsSignature requires SameAs')
            bcm.require(len(tag_element) == 1, 'expected exactly one tag type')
            children(tag_element, optional=types.keys())
            type_element = tag_element[0]
            block = read_block(type_element, types[type_element.tag], tag)
            document['blocks'].append(block)
            name = block['name']
        names[tag] = name
        document['tags'].append({'id': tag, 'block': name})
    bcm.encode(document)
    return document


def loads(text):
    try:
        return parse_document(text)
    except bcm.FormatError:
        raise
    except ValueError as error:
        raise bcm.FormatError(str(error)) from error


def dumps(document):
    bcm.encode(document)
    blocks = {block['name']: block for block in document['blocks']}
    first_use = list(dict.fromkeys(tag['block'] for tag in document['tags']))
    bcm.require(first_use == list(blocks), 'physical block order differs from first directory use')
    root = ET.Element('IccProfile')
    root.append(ET.Comment(' IccXML with BCM extensions; see README.md. Original data notices: PROVENANCE.md. '))
    header = add(root, 'Header')
    for name, tag in HEADER_NAMES.items():
        value = document['header'][name]
        if name == 'created':
            year, month, day, hour, minute, second = bcm.creation_stamp(value)
            value = f'{year:04d}-{month:02d}-{day:02d}T{hour:02d}:{minute:02d}:{second:02d}'
        add_text(header, tag, value)
    if document.get('directory_padding'):
        add(header, EXT + 'DirectoryPadding', document['directory_padding'])
    tags = add(root, 'Tags')
    defined = {}
    for item in document['tags']:
        tag = bcm.tag_number(item['id'])
        name = item['block']
        tag_name = TAG_NAMES.get(tag, 'PrivateTag')
        attrs = {'TagSignature': f'{tag:08X}h'} if tag_name == 'PrivateTag' else {}
        element = add(tags, tag_name, **attrs)
        if name in defined:
            target = defined[name]
            element.set('SameAs', TAG_NAMES.get(target, 'PrivateTag'))
            if target not in TAG_NAMES:
                element.set('SameAsSignature', f'{target:08X}h')
        else:
            defined[name] = tag
            block = blocks[name]
            write_block(add(element, TYPE_NAMES[block['kind']]), block)
    ET.indent(root, space='  ')
    # XML parsers normalize literal CR; references preserve it. Tabs remain
    # visible even in the trailing whitespace of Brother's text copies.
    text = ET.tostring(root, encoding='unicode').replace('\r', '&#13;').replace('\t', '&#9;')
    text = '<?xml version="1.0" encoding="UTF-8"?>\n' + text + '\n'
    bcm.require(len(text.encode('utf-8')) <= bcm.MAX_BYTES, 'description exceeds 64 MiB')
    return text


def load_text(path):
    return loads(bcm.read_limited(path).decode('utf-8'))
