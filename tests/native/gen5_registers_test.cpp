// Copyright (C) 2026 NebulaAPU contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// The register bank is the point of the port, so what is tested is the
// traffic it saves and not merely that it returns something. The numbers
// are compared against what the dispatcher design is forced into: one
// access chain, one load and one store per instruction.
#include <cstdio>

#include "gen5_registers.h"

static int failures = 0;

static void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

struct Fixture {
    ps5spirv::ModuleBuilder module;
    ps5gen5::RegisterBank bank;

    Fixture() {
        const auto uint_type = module.type_int(32, false);
        const auto pointer = module.type_pointer(
            ps5spirv::StorageClass::Private, uint_type);
        const auto length = module.constant(uint_type, 256);
        const auto array = module.type_array(uint_type, length);
        const auto array_pointer = module.type_pointer(
            ps5spirv::StorageClass::Private, array);
        const auto variable = module.allocate_id();
        module.add_global(
            ps5spirv::Op::Variable,
            {array_pointer, variable,
             static_cast<std::uint32_t>(ps5spirv::StorageClass::Private)});
        bank = ps5gen5::RegisterBank(
            &module, variable, pointer, uint_type, uint_type);
    }
};

int main() {
    // Reading the same register twice loads once. The dispatcher cannot do
    // this: between two instructions control may have left the block.
    {
        Fixture f;
        const auto first = f.bank.read(7);
        const auto second = f.bank.read(7);
        check(first == second, "a second read gives the same value");
        check(f.bank.stats().loads_emitted == 1, "only one load is emitted");
        check(f.bank.stats().reads_served_from_cache == 1,
              "the second read is served without memory");
    }

    // Reading two registers loads twice - the cache is per register, not a
    // single slot.
    {
        Fixture f;
        f.bank.read(1);
        f.bank.read(2);
        check(f.bank.stats().loads_emitted == 2,
              "different registers load separately");
    }

    // A write touches no memory, and a read after it sees the value
    // written rather than loading the old one.
    {
        Fixture f;
        f.bank.write(3, 42);
        const auto value = f.bank.read(3);
        check(value == 42, "a read after a write sees what was written");
        check(f.bank.stats().loads_emitted == 0,
              "a write means the old value is never loaded");
    }

    // A register written repeatedly is stored once.
    {
        Fixture f;
        f.bank.write(5, 10);
        f.bank.write(5, 11);
        f.bank.write(5, 12);
        f.bank.flush();
        check(f.bank.stats().stores_emitted == 1,
              "three writes become one store");
    }

    // A register only read is not stored, which is most of them.
    {
        Fixture f;
        f.bank.read(9);
        f.bank.flush();
        check(f.bank.stats().stores_emitted == 0,
              "reading does not make a register dirty");
    }

    // After a flush the next block starts clean: a value from the previous
    // block must not be used, because another block may have written it.
    {
        Fixture f;
        f.bank.write(4, 77);
        f.bank.flush();
        check(!f.bank.is_live(4), "a flush ends the block's knowledge");
        f.bank.read(4);
        check(f.bank.stats().loads_emitted == 1,
              "the next block loads the register again");
    }

    // The shape of the saving, on a block that does what a real one does:
    // several instructions over a few registers. The dispatcher pays a load
    // and a store per instruction; this pays one of each per register.
    {
        Fixture f;
        // r1 = r0 + r0; r2 = r1 + r0; r1 = r2 + r1; r2 = r1 + r2
        const auto r0 = f.bank.read(0);
        f.bank.write(1, r0);
        const auto r1 = f.bank.read(1);
        f.bank.write(2, r1);
        const auto r2 = f.bank.read(2);
        f.bank.write(1, r2);
        f.bank.write(2, f.bank.read(1));
        f.bank.flush();
        check(f.bank.stats().loads_emitted == 1,
              "eight register accesses load once");
        check(f.bank.stats().stores_emitted == 2,
              "two registers written means two stores");
    }

    // The scalar file's top half is mostly constants. Reading one loads
    // nothing and gives the value the encoding stands for; reading the
    // register numbered like it would give whatever the driver left in an
    // unwritten variable.
    {
        Fixture f;
        f.bank.set_encodes_constants(true);
        const auto uint_type = f.module.type_int(32, false);
        f.bank.set_literal(0x12345678u);
        check(f.bank.read(0x83) == f.module.constant(uint_type, 3),
              "encoding 0x83 is the integer three");
        check(f.bank.read(0xC1) == f.module.constant(uint_type, 0xFFFFFFFFu),
              "encoding 0xC1 is minus one");
        check(f.bank.read(0xF2) == f.module.constant(uint_type, 0x3F800000u),
              "encoding 0xF2 is the float one");
        check(f.bank.read(0xFF) == f.module.constant(uint_type, 0x12345678u),
              "encoding 0xFF is the instruction's literal");
        check(f.bank.read_high(0xC1) ==
                  f.module.constant(uint_type, 0xFFFFFFFFu),
              "minus one is minus one in both halves");
        check(f.bank.stats().loads_emitted == 0,
              "and none of them loads");
        f.bank.read(0x7E);
        check(f.bank.stats().loads_emitted == 1,
              "while exec is still a register");
    }

    if (failures == 0) {
        std::printf("all register bank checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
