# Copyright (C) 2026 NebulaAPU contributors
# SPDX-License-Identifier: GPL-2.0-only

"""A validator for the subset of SPIR-V the native translator emits.

Not a general one. It knows the shape of every opcode the translator can
produce - whether it has a result type and a result id, and which of its
operands are ids - and checks the things a driver will fault on rather than
report: an id used before it is defined, a result id defined twice, an id
at or above the bound, a block that does not start with a label or does not
end with a terminator, and an instruction after a terminator.

Point it at a directory of modules dumped by setting
PS5RT_NATIVE_SHADER_DUMP to a directory while the native producer runs.

It is worth what it does not find as much as what it does: every module
this title produces passes it, and the runtime still faults when one of
them is actually executed, so the fault is semantic - types, decorations,
structured control flow rules, or a disagreement with the descriptor set
the runtime builds - and not one of the structural mistakes a hand-written
builder usually makes.
"""
import struct
import sys
import glob
import os

MAGIC = 0x07230203

# opcode: (has_result_type, has_result_id, id_operand_indexes or 'all',
#          trailing_literals)
# Indexes are into the operand list after the result type and result id.
SHAPES = {
    3: (False, False, [], 'string'),      # Source
    5: (False, False, [0], 'string'),     # Name
    6: (False, False, [0], 'string'),     # MemberName
    11: (False, True, [], 'string'),      # ExtInstImport
    12: (True, True, 'ext', None),        # ExtInst
    14: (False, False, [], None),         # MemoryModel
    15: (False, False, 'entry', 'string'),  # EntryPoint
    16: (False, False, [0], None),        # ExecutionMode
    17: (False, False, [], None),         # Capability
    19: (False, True, [], None),          # TypeVoid
    20: (False, True, [], None),          # TypeBool
    21: (False, True, [], None),          # TypeInt
    22: (False, True, [], None),          # TypeFloat
    23: (False, True, [0], None),         # TypeVector
    25: (False, True, [0], None),         # TypeImage
    27: (False, True, [0], None),         # TypeSampledImage
    28: (False, True, [0, 1], None),      # TypeArray
    29: (False, True, [0], None),         # TypeRuntimeArray
    30: (False, True, 'all', None),       # TypeStruct
    32: (False, True, [1], None),         # TypePointer
    33: (False, True, 'all', None),       # TypeFunction
    43: (True, True, [], None),           # Constant
    54: (True, True, [1], None),          # Function
    56: (False, False, [], None),         # FunctionEnd
    59: (True, True, [], None),           # Variable (storage is a literal)
    61: (True, True, [0], None),          # Load
    62: (False, False, [0, 1], None),     # Store
    65: (True, True, 'all', None),        # AccessChain
    71: (False, False, [0], None),        # Decorate
    72: (False, False, [0], None),        # MemberDecorate
    80: (True, True, 'all', None),        # CompositeConstruct
    81: (True, True, [0], None),          # CompositeExtract
    86: (True, True, [0, 1], None),       # SampledImage
    88: (True, True, 'image', None),      # ImageSampleExplicitLod
    95: (True, True, 'image', None),      # ImageFetch
    98: (True, True, 'image', None),      # ImageRead
    99: (False, False, [0, 1, 2], None),  # ImageWrite
    124: (True, True, [0], None),         # Bitcast
    169: (True, True, [0, 1, 2], None),   # Select
    246: (False, False, [0, 1], None),    # LoopMerge
    247: (False, False, [0], None),       # SelectionMerge
    248: (False, True, [], None),         # Label
    249: (False, False, [0], None),       # Branch
    250: (False, False, [0, 1, 2], None), # BranchConditional
    253: (False, False, [], None),        # Return
    254: (False, False, [], None),        # ReturnValue
    255: (False, False, [], None),        # Unreachable
}

TERMINATORS = {249, 250, 253, 254, 255, 252}

# Everything else the translator emits is an arithmetic or comparison
# instruction: result type, result id, then all ids.
def shape_of(opcode):
    if opcode in SHAPES:
        return SHAPES[opcode]
    return (True, True, 'all', None)


def validate(path):
    with open(path, 'rb') as handle:
        data = handle.read()
    words = list(struct.unpack('<%dI' % (len(data) // 4), data))
    problems = []
    if len(words) < 5 or words[0] != MAGIC:
        return ['not a SPIR-V module']
    bound = words[3]
    # Labels first. A branch may name a block that comes later, which is
    # legal and is not a forward reference to be complained about.
    at = 5
    defined = set()
    labels = set()
    while at < len(words):
        head = words[at]
        length = head >> 16
        if length == 0 or at + length > len(words):
            break
        if (head & 0xFFFF) == 248:
            labels.add(words[at + 1])
        at += length

    at = 5
    in_function = False
    block_open = False
    seen_terminator = False
    index = 0
    while at < len(words):
        head = words[at]
        length = head >> 16
        opcode = head & 0xFFFF
        if length == 0 or at + length > len(words):
            problems.append('instruction %d: length %d runs past the end'
                            % (index, length))
            break
        operands = words[at + 1:at + length]
        has_type, has_result, id_positions, trailing = shape_of(opcode)
        cursor = 0
        used = []
        if has_type:
            used.append(operands[cursor])
            cursor += 1
        result = None
        if has_result:
            result = operands[cursor]
            cursor += 1
        rest = operands[cursor:]
        if id_positions == 'all':
            used.extend(rest)
        elif id_positions == 'ext':
            # set id, then a literal instruction number, then ids
            if rest:
                used.append(rest[0])
                used.extend(rest[2:])
        elif id_positions == 'image':
            # image, coordinate, literal operand mask, then ids
            used.extend(rest[:2])
            used.extend(rest[3:])
        elif id_positions == 'entry':
            # model, function, then the interface ids, then a string
            if len(rest) > 1:
                used.append(rest[1])
        elif isinstance(id_positions, list):
            for position in id_positions:
                if position < len(rest):
                    used.append(rest[position])

        for identifier in used:
            if identifier == 0 or identifier >= bound:
                problems.append(
                    'instruction %d (op %d) uses id %d, bound is %d'
                    % (index, opcode, identifier, bound))
            elif (identifier not in defined and identifier not in labels
                  and in_function):
                problems.append(
                    'instruction %d (op %d) uses id %d before it is defined'
                    % (index, opcode, identifier))
        if result is not None:
            if result in defined and opcode != 248:
                problems.append(
                    'instruction %d (op %d) defines id %d a second time'
                    % (index, opcode, result))
            defined.add(result)

        if opcode == 54:
            in_function = True
            block_open = False
            seen_terminator = False
        elif opcode == 56:
            if block_open and not seen_terminator:
                problems.append(
                    'instruction %d: function ends with an open block'
                    % index)
            in_function = False
        elif in_function:
            if opcode == 248:
                if block_open and not seen_terminator:
                    problems.append(
                        'instruction %d: a label starts while the previous '
                        'block has no terminator' % index)
                block_open = True
                seen_terminator = False
            elif not block_open:
                problems.append(
                    'instruction %d (op %d) is outside any block'
                    % (index, opcode))
            elif seen_terminator:
                problems.append(
                    'instruction %d (op %d) follows a terminator'
                    % (index, opcode))
            if opcode in TERMINATORS:
                seen_terminator = True
        at += length
        index += 1
    return problems


def structured_control_flow(words):
    """Whether each construct has a merge block of its own.

    SPIR-V allows a block to be the merge block of exactly one header. Two
    nested selections that leave through the same block therefore cannot
    both name it, however natural that is to arrive at: the immediate post
    dominator of two nested branches is often the same block.

    A driver is entitled to do anything with a module that breaks this, and
    at least one does - it faults inside vkCreateComputePipelines, taking
    the process with it, on a module with 54 such collisions while
    compiling one with 2 without complaint.
    """
    at = 5
    blocks = []
    current = None
    while at < len(words):
        head = words[at]
        length = head >> 16
        opcode = head & 0xFFFF
        if length == 0 or at + length > len(words):
            break
        operands = words[at + 1:at + length]
        if opcode == 248:
            current = {'label': operands[0], 'merge': None, 'cont': None}
            blocks.append(current)
        elif current is not None:
            if opcode == 246:
                current['merge'] = operands[0]
                current['cont'] = operands[1]
            elif opcode == 247:
                current['merge'] = operands[0]
        at += length
    problems = []
    claimed = {}
    for block in blocks:
        merge = block['merge']
        if merge is None:
            continue
        if merge in claimed:
            problems.append(
                'block %d is the merge of both %d and %d'
                % (merge, claimed[merge], block['label']))
        claimed[merge] = block['label']
    return problems


def main():
    target = sys.argv[1] if len(sys.argv) > 1 else '.'
    files = sorted(glob.glob(os.path.join(target, '*.spv')))
    if not files:
        print('no modules in %s' % target)
        return 1
    bad = 0
    for path in files:
        with open(path, 'rb') as handle:
            data = handle.read()
        words = list(struct.unpack('<%dI' % (len(data) // 4), data))
        problems = validate(path) + structured_control_flow(words)
        name = os.path.basename(path)
        if problems:
            bad += 1
            print('%s: %d problems' % (name, len(problems)))
            for problem in problems[:5]:
                print('    %s' % problem)
        else:
            print('%s: sound' % name)
    print('%d of %d modules have problems' % (bad, len(files)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
