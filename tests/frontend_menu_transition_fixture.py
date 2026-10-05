"""Independent original Main/Options transition bytecode fixtures."""
import struct


def script():
    op = lambda code, value=0: (code << 11) | value
    strings = b'startmainmenuball\0fechoosecaptains\0outofballcam\0'
    options = len(b'startmainmenuball\0')
    back = options + len(b'fechoosecaptains\0')
    functions = [
        (0x0017a6dd, [op(8, 8), op(10)]),
        (0x1b0db3b8, [op(8, 47), op(2, 95), op(8, 5), op(3, 0),
                      op(0, 0), op(2, 0), op(8, 23), op(8, 44), op(8, 46),
                      op(2, 0), op(8, 21), op(8, 41), op(10)]),
        (0x6f23e5f3, [op(3, options), op(2, 0), op(2, 0), op(8, 23),
                      op(2, 1), op(8, 35), op(2, 13), op(2, 1), op(8, 25), op(10)]),
        (0xc18f9013, [op(8, 42), op(3, back), op(2, 0), op(2, 1), op(8, 23),
                      op(8, 44), op(0, 1), op(8, 21), op(8, 46),
                      op(2, 1), op(2, 2), op(8, 25), op(10)]),
    ]
    code = []
    table = bytearray()
    for hash_value, instructions in functions:
        table.extend(struct.pack('>IIHBB', hash_value, len(code)*2, 2, 0, 0))
        code.extend(instructions)
    header = [0xe11c2112, len(functions), 0, 0, 8, len(code)*2,
              len(strings), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
    return (struct.pack('>18I', *header)+table+struct.pack('>2f', .25, 1)
            +struct.pack('>'+str(len(code))+'H', *code)+strings)
