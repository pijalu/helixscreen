// SPDX-License-Identifier: GPL-3.0-or-later
// Tests for server-side symbol resolution.

import { describe, it, expect } from "vitest";
import {
  parseSymbolTable,
  lookupSymbol,
  autoDetectLoadBase,
  resolveBacktrace,
  isSharedLibAddr,
} from "../symbol-resolver";

// ---------- Sample nm -nC output ----------

const SAMPLE_NM_OUTPUT = `00010000 T _start
00010100 T _init
00020000 T main
00020200 T Application::run()
00020500 t Application::handle_signal(int)
00030000 W std::vector<int>::push_back(int const&)
00040000 D global_data
00050000 B bss_section
00060000 T PrinterState::update()
00060200 T PrinterState::connect()
`;

// ---------- parseSymbolTable ----------

describe("parseSymbolTable", () => {
  it("parses standard nm output into sorted symbol array", () => {
    const symbols = parseSymbolTable(SAMPLE_NM_OUTPUT);
    expect(symbols.length).toBe(8); // T, t, W only (not D, B)
    expect(symbols[0]).toEqual({ address: 0x10000, type: "T", name: "_start" });
    expect(symbols[1]).toEqual({ address: 0x10100, type: "T", name: "_init" });
    expect(symbols[2]).toEqual({ address: 0x20000, type: "T", name: "main" });
  });

  it("filters out non-text symbol types (D, B, etc.)", () => {
    const symbols = parseSymbolTable(SAMPLE_NM_OUTPUT);
    const types = symbols.map((s) => s.type);
    expect(types).not.toContain("D");
    expect(types).not.toContain("B");
    expect(types).toContain("T");
    expect(types).toContain("t");
    expect(types).toContain("W");
  });

  it("handles demangled names with spaces", () => {
    const symbols = parseSymbolTable(SAMPLE_NM_OUTPUT);
    const pushBack = symbols.find((s) => s.name.includes("push_back"));
    expect(pushBack).toBeDefined();
    expect(pushBack!.name).toBe("std::vector<int>::push_back(int const&)");
  });

  it("skips malformed lines", () => {
    const input = `not a valid line
00010000 T _start
another bad line

00020000 T main`;
    const symbols = parseSymbolTable(input);
    expect(symbols.length).toBe(2);
    expect(symbols[0].name).toBe("_start");
    expect(symbols[1].name).toBe("main");
  });

  it("handles empty input", () => {
    expect(parseSymbolTable("")).toEqual([]);
    expect(parseSymbolTable("\n\n\n")).toEqual([]);
  });

  it("filters LTO compilation-unit section markers", () => {
    const input = `00010000 T _start
00020000 T main
1bdf2f2c t lv_i18n_translations.c.35cb4f60
00030000 T real_function
004dd6c0 t tvgSwRle.cpp.a1b2c3d4`;
    const symbols = parseSymbolTable(input);
    const names = symbols.map((s) => s.name);
    expect(names).not.toContain("lv_i18n_translations.c.35cb4f60");
    expect(names).not.toContain("tvgSwRle.cpp.a1b2c3d4");
    expect(names).toContain("_start");
    expect(names).toContain("main");
    expect(names).toContain("real_function");
  });
});

// ---------- lookupSymbol ----------

describe("lookupSymbol", () => {
  const symbols = [
    { address: 0x1000, type: "T", name: "func_a" },
    { address: 0x2000, type: "T", name: "func_b" },
    { address: 0x3000, type: "T", name: "func_c" },
    { address: 0x4000, type: "T", name: "func_d" },
  ];

  it("finds exact match", () => {
    const result = lookupSymbol(symbols, 0x2000);
    expect(result!.name).toBe("func_b");
  });

  it("finds symbol when address is between two symbols", () => {
    const result = lookupSymbol(symbols, 0x2500);
    expect(result!.name).toBe("func_b");
  });

  it("returns null when address is before first symbol", () => {
    const result = lookupSymbol(symbols, 0x500);
    expect(result).toBeNull();
  });

  it("finds last symbol when address is after all symbols", () => {
    const result = lookupSymbol(symbols, 0x5000);
    expect(result!.name).toBe("func_d");
  });

  it("returns null for empty array", () => {
    expect(lookupSymbol([], 0x1000)).toBeNull();
  });

  it("handles single-element array", () => {
    const single = [{ address: 0x1000, type: "T", name: "only" }];
    expect(lookupSymbol(single, 0x1000)!.name).toBe("only");
    expect(lookupSymbol(single, 0x1500)!.name).toBe("only");
    expect(lookupSymbol(single, 0x500)).toBeNull();
  });
});

// ---------- autoDetectLoadBase ----------

describe("autoDetectLoadBase", () => {
  const symbols = parseSymbolTable(SAMPLE_NM_OUTPUT);

  it("detects load base from _start/main gap in backtrace", () => {
    // _start = 0x10000, main = 0x20000 in file
    // Simulate ASLR: load_base = 0xAAAA0000
    const loadBase = 0xaaaa0000;
    const backtraceAddrs = [
      `0x${(loadBase + 0x20200).toString(16)}`, // Application::run
      `0x${(loadBase + 0x20000).toString(16)}`, // main
      `0x${(loadBase + 0x10000).toString(16)}`, // _start
    ];

    const detected = autoDetectLoadBase(symbols, backtraceAddrs);
    expect(detected).toBe(loadBase);
  });

  it("returns null when _start is not in symbol table", () => {
    const noStart = symbols.filter((s) => s.name !== "_start");
    const result = autoDetectLoadBase(noStart, ["0x100000", "0x200000"]);
    expect(result).toBeNull();
  });

  it("returns null when main is not in symbol table", () => {
    const noMain = symbols.filter((s) => s.name !== "main");
    const result = autoDetectLoadBase(noMain, ["0x100000", "0x200000"]);
    expect(result).toBeNull();
  });

  it("returns null when backtrace has no matching gap", () => {
    const result = autoDetectLoadBase(symbols, [
      "0xDEAD0001",
      "0xDEAD0002",
      "0xDEAD0003",
    ]);
    expect(result).toBeNull();
  });

  it("returns null for empty backtrace", () => {
    expect(autoDetectLoadBase(symbols, [])).toBeNull();
  });
});

// ---------- isSharedLibAddr ----------

describe("isSharedLibAddr", () => {
  const symbols = parseSymbolTable(SAMPLE_NM_OUTPUT);

  it("returns true for negative file offsets", () => {
    // Negative offset means address was below load_base (shared lib region)
    expect(isSharedLibAddr(symbols, -0x1000)).toBe(true);
  });

  it("returns true for addresses far beyond the last symbol", () => {
    // 0x7fff80012345 is way beyond any symbol in our binary
    expect(isSharedLibAddr(symbols, 0x7fff80012345)).toBe(true);
  });

  it("returns false for addresses within the binary range", () => {
    expect(isSharedLibAddr(symbols, 0x20200)).toBe(false); // Application::run
    expect(isSharedLibAddr(symbols, 0x10000)).toBe(false); // _start
  });

  it("returns false for addresses slightly past last symbol", () => {
    // Last symbol is PrinterState::connect() at 0x60200
    // A small offset past it should still be considered in-binary
    expect(isSharedLibAddr(symbols, 0x60300)).toBe(false);
  });

  it("returns true for empty symbol table", () => {
    expect(isSharedLibAddr([], 0x1000)).toBe(true);
  });
});

// ---------- resolveBacktrace ----------

describe("resolveBacktrace", () => {
  function createMockBucket(files: Record<string, string> = {}): R2Bucket {
    return {
      async get(key: string) {
        const content = files[key];
        if (!content) return null;
        return {
          text: async () => content,
        };
      },
    } as unknown as R2Bucket;
  }

  const symFileContent = SAMPLE_NM_OUTPUT;

  it("resolves backtrace with explicit load_base", async () => {
    const bucket = createMockBucket({
      "symbols/v0.9.9/pi.sym": symFileContent,
    });

    const report = {
      app_version: "0.9.9",
      platform: "pi",
      load_base: "0x0",
      backtrace: ["0x00020200", "0x00020000", "0x00010000"],
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(true);
    expect(result.frames).toHaveLength(3);
    expect(result.frames[0].symbol).toBe("Application::run()+0x0");
    expect(result.frames[1].symbol).toBe("main+0x0");
    expect(result.frames[2].symbol).toBe("_start+0x0");
  });

  it("resolves backtrace with non-zero load_base", async () => {
    const bucket = createMockBucket({
      "symbols/v0.9.9/pi.sym": symFileContent,
    });

    const loadBase = 0xaaaa0000;
    const report = {
      app_version: "0.9.9",
      platform: "pi",
      load_base: `0x${loadBase.toString(16)}`,
      backtrace: [`0x${(loadBase + 0x20200).toString(16)}`],
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(true);
    expect(result.loadBase).toBe(`0x${loadBase.toString(16)}`);
    expect(result.frames[0].symbol).toBe("Application::run()+0x0");
  });

  it("auto-detects load_base when not provided", async () => {
    const bucket = createMockBucket({
      "symbols/v0.9.9/pi.sym": symFileContent,
    });

    const loadBase = 0xbbbb0000;
    const report = {
      app_version: "0.9.9",
      platform: "pi",
      backtrace: [
        `0x${(loadBase + 0x20200).toString(16)}`, // Application::run
        `0x${(loadBase + 0x20000).toString(16)}`, // main
        `0x${(loadBase + 0x10000).toString(16)}`, // _start
      ],
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(true);
    expect(result.autoDetectedBase).toBe(true);
    expect(result.loadBase).toBe(`0x${loadBase.toString(16)}`);
    expect(result.frames[0].symbol).toBe("Application::run()+0x0");
  });

  it("returns symbolFileFound: false when sym file missing", async () => {
    const bucket = createMockBucket({}); // empty bucket

    const report = {
      app_version: "0.9.9",
      platform: "pi",
      backtrace: ["0x00020200"],
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(false);
    expect(result.frames).toEqual([]);
  });

  it("resolves registers", async () => {
    const bucket = createMockBucket({
      "symbols/v0.9.9/pi.sym": symFileContent,
    });

    const report = {
      app_version: "0.9.9",
      platform: "pi",
      load_base: "0x0",
      backtrace: ["0x00020200"],
      registers: {
        pc: "0x00060000",
        lr: "0x00020200",
        sp: "0x7ffff000",
      },
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.resolvedRegisters).toBeDefined();
    expect(result.resolvedRegisters!.pc).toBe("PrinterState::update()+0x0");
    expect(result.resolvedRegisters!.lr).toBe("Application::run()+0x0");
    // SP (0x7ffff000) is way past all code symbols — the resolver correctly
    // filters it out as an implausibly large offset (shared lib / stack space)
    expect(result.resolvedRegisters!.sp).toBeUndefined();
  });

  it("handles missing version gracefully", async () => {
    const bucket = createMockBucket({});
    const report = { platform: "pi", backtrace: ["0x1234"] };
    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(false);
  });

  it("handles missing platform gracefully", async () => {
    const bucket = createMockBucket({});
    const report = { app_version: "0.9.9", backtrace: ["0x1234"] };
    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(false);
  });

  it("uses app_platform fallback field", async () => {
    const bucket = createMockBucket({
      "symbols/v0.9.9/pi.sym": symFileContent,
    });

    const report = {
      app_version: "0.9.9",
      app_platform: "pi",
      load_base: "0x0",
      backtrace: ["0x00020000"],
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(true);
    expect(result.frames[0].symbol).toBe("main+0x0");
  });

  it("handles address with offset correctly", async () => {
    const bucket = createMockBucket({
      "symbols/v0.9.9/pi.sym": symFileContent,
    });

    const report = {
      app_version: "0.9.9",
      platform: "pi",
      load_base: "0x0",
      backtrace: ["0x00020123"], // main+0x123
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.frames[0].symbol).toBe("main+0x123");
  });

  it("never throws even with bad bucket", async () => {
    const bucket = {
      get: async () => {
        throw new Error("R2 exploded");
      },
    } as unknown as R2Bucket;

    const report = {
      app_version: "0.9.9",
      platform: "pi",
      backtrace: ["0x1234"],
    };

    // Should not throw
    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(false);
  });

  it("labels shared library addresses instead of producing garbage symbols", async () => {
    const bucket = createMockBucket({
      "symbols/v0.9.9/pi.sym": symFileContent,
    });

    // Simulate a real crash: some frames in our binary, some in libc.
    // load_base = 0xaaaab000, libc address = 0x7fff80012345 (way outside binary).
    const loadBase = 0xaaaab000;
    const report = {
      app_version: "0.9.9",
      platform: "pi",
      load_base: `0x${loadBase.toString(16)}`,
      backtrace: [
        `0x${(loadBase + 0x20200).toString(16)}`, // Application::run (in binary)
        "0x7fff80012345", // libc address (shared library)
        `0x${(loadBase + 0x20000).toString(16)}`, // main (in binary)
        "0x7fff80098765", // another libc address
      ],
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(true);
    expect(result.frames).toHaveLength(4);
    expect(result.frames[0].symbol).toBe("Application::run()+0x0");
    expect(result.frames[1].symbol).toBe("<shared library>");
    expect(result.frames[2].symbol).toBe("main+0x0");
    expect(result.frames[3].symbol).toBe("<shared library>");
  });

  it("treats load_base '0x0' as file-relative addresses without auto-detect override", async () => {
    const bucket = createMockBucket({
      "symbols/v0.9.9/pi.sym": symFileContent,
    });

    // Simulate static-PIE ARM32: backtrace() returns file-relative addresses,
    // load_base explicitly reported as 0x0
    const report = {
      app_version: "0.9.9",
      platform: "pi",
      load_base: "0x0",
      backtrace: ["0x00020200", "0x00020000"],
    };

    const result = await resolveBacktrace(bucket, report);
    expect(result.symbolFileFound).toBe(true);
    // Should NOT try auto-detection — load_base was explicitly provided
    expect(result.autoDetectedBase).toBe(false);
    expect(result.frames[0].symbol).toBe("Application::run()+0x0");
    expect(result.frames[1].symbol).toBe("main+0x0");
  });
});
