#include "staze/diagnostic.hpp"
#include "staze/lexer.hpp"
#include "staze/llvm_x64.hpp"
#include "staze/parser.hpp"
#include "staze/semantic.hpp"
#include "staze/sir.hpp"
#include "staze/toolchain.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

static std::string read_all(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open input: " + p.string());
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

static void write_all(const fs::path& p, const std::string& s) {
    if (!p.parent_path().empty()) fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot write output: " + p.string());
    f << s;
}

static void usage() {
    std::cout <<
R"(Staze C++23 Compiler 0.8.0 — transactional undo, structured tasks & SIR-C 6.0

Source compilation:
  stazec <file.stz3> [-o program.exe] [--check]
                     [--emit-sir-s file.sirs]
                     [--emit-sir-c file.sirc]
                     [--emit-sir file.sir]
                     [--emit-llvm file.ll]
                     [--keep-intermediates]

Detached semantic compilation from a serialized SIR-S artifact:
  stazec --from-sir-s <file.sirs> [-o program.exe] [--check]
                                  [--emit-sir-c file.sirc]
                                  [--emit-llvm file.ll]
                                  [--keep-intermediates]

Backend-only compilation from a serialized SIR-C artifact:
  stazec --from-sir-c <file.sirc> [-o program.exe] [--check]
                                  [--emit-llvm file.ll]
                                  [--keep-intermediates]

The source path is deliberately severed before LLVM:
  .stz3 -> AST -> SSL -> DLE -> SIR-S 5.x
       -> serialize -> deserialize -> independent SIR-S verifier
       -> SIR-C 6.0 -> serialize -> deserialize -> independent SIR-C verifier
       -> LLVM IR -> x86-64 COFF -> PE32+ .exe

SIR-S remains semantic meaning. SIR-C 6.0 adds executable, verifier-backed
transaction undo plans and structured task/capture plans on top of the 0.7
ownership/resource model. The Windows x86-64 reference task scheduler is
structured and deterministic: task regions execute in declared order and join
before parallel exit. No hidden thread runtime is required by this profile.

Reference transactions are deliberately single-pass in 0.8: nested transactions,
loops inside a transaction, irreversible release, and dynamic-Many mutation inside
a transaction are rejected until a bounded undo-log profile exists. Supported
field/resource/shared/ownership effects carry explicit inverse plans that execute
on abort and are disarmed on commit.

The --from-sir-s path proves target concretization can operate without source,
tokens, AST, or SSL. The --from-sir-c path proves LLVM/COFF/PE generation can
operate without any frontend or target-neutral lowering state at all.

Environment overrides:
  STAZE_CLANG      path/name of clang
  STAZE_LLD_LINK   path/name of lld-link
)";
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    fs::path input;
    try {
        fs::path output;
        fs::path sirs_out;
        fs::path sirc_out;
        fs::path combined_sir_out;
        fs::path llvm_out;
        bool check_only = false;
        bool keep = false;
        bool from_sirc = false;
        bool from_sirs = false;

        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a == "-h" || a == "--help") { usage(); return 0; }
            if (a == "-o") { if (++i >= argc) throw std::runtime_error("-o requires a path"); output = argv[i]; continue; }
            if (a == "--emit-sir-s") { if (++i >= argc) throw std::runtime_error("--emit-sir-s requires a path"); sirs_out = argv[i]; continue; }
            if (a == "--emit-sir-c") { if (++i >= argc) throw std::runtime_error("--emit-sir-c requires a path"); sirc_out = argv[i]; continue; }
            if (a == "--emit-sir") { if (++i >= argc) throw std::runtime_error("--emit-sir requires a path"); combined_sir_out = argv[i]; continue; }
            if (a == "--emit-llvm") { if (++i >= argc) throw std::runtime_error("--emit-llvm requires a path"); llvm_out = argv[i]; continue; }
            if (a == "--check") { check_only = true; continue; }
            if (a == "--keep-intermediates") { keep = true; continue; }
            if (a == "--from-sir-s") {
                from_sirs = true;
                if (++i >= argc) throw std::runtime_error("--from-sir-s requires a path");
                if (!input.empty()) throw std::runtime_error("only one input artifact is accepted");
                input = argv[i];
                continue;
            }
            if (a == "--from-sir-c") {
                from_sirc = true;
                if (++i >= argc) throw std::runtime_error("--from-sir-c requires a path");
                if (!input.empty()) throw std::runtime_error("only one input artifact is accepted");
                input = argv[i];
                continue;
            }
            if (!a.empty() && a[0] == '-') throw std::runtime_error("unknown option: " + a);
            if (!input.empty()) throw std::runtime_error("only one input source/artifact is accepted");
            input = a;
        }

        if (input.empty()) throw std::runtime_error("missing .stz3, .sirs, or .sirc input");
        if (from_sirs && from_sirc) throw std::runtime_error("choose only one detached input mode");
        if (!from_sirs && !from_sirc && input.extension() == ".sirs") from_sirs = true;
        if (!from_sirs && !from_sirc && input.extension() == ".sirc") from_sirc = true;
        if (from_sirc && !sirs_out.empty()) throw std::runtime_error("--emit-sir-s is unavailable when SIR-S was intentionally skipped");
        if (output.empty()) { output = input.filename(); output.replace_extension(".exe"); }

        staze::SirVerifier verifier;
        staze::SirCProgram backend_sir_c;
        std::string canonical_sirs;
        std::string canonical_sirc;

        if (from_sirc) {
            // Backend-only process path. No Lexer, Parser, Program, SSLProgram,
            // DeductiveLowerer, or SIR-S object is required.
            backend_sir_c = staze::deserialize_sir_c(read_all(input));
            verifier.verify(backend_sir_c);
            canonical_sirc = staze::serialize_sir_c(backend_sir_c);
        } else {
            staze::SirSProgram detached_sir_s;
            if (from_sirs) {
                // Detached target-neutral path. Concretization starts from bytes;
                // source, tokens, AST, SSL, and DLE never exist in this process.
                detached_sir_s = staze::deserialize_sir_s(read_all(input));
                verifier.verify(detached_sir_s);
                canonical_sirs = staze::serialize_sir_s(detached_sir_s);
            } else {
                // FRONTEND LIFETIME ZONE. These objects deliberately die before
                // target concretization and LLVM emission.
                const auto source = read_all(input);
                staze::Lexer lexer(source);
                auto tokens = lexer.lex_all();
                staze::Parser parser(std::move(tokens));
                auto ast = parser.parse_program();
                staze::SemanticAnalyzer analyzer;
                auto ssl = analyzer.analyze(ast);
                staze::DeductiveLowerer dle;
                auto frontend_sir_s = dle.lower(ast, ssl);
                verifier.verify(frontend_sir_s);

                // The serialization boundary is architectural, not decorative.
                // The retained SIR-S is reconstructed from bytes, so it cannot
                // contain a hidden AST/SSL pointer even by accident.
                canonical_sirs = staze::serialize_sir_s(frontend_sir_s);
                detached_sir_s = staze::deserialize_sir_s(canonical_sirs);
                if (staze::serialize_sir_s(detached_sir_s) != canonical_sirs)
                    throw std::runtime_error("internal: SIR-S canonical serialization is not byte-stable after round-trip");
            }

            // At this line source/tokens/AST/SSL are absent regardless of whether
            // the input was source or a detached SIR-S artifact.
            verifier.verify(detached_sir_s);
            staze::TargetConcretizer concretizer;
            auto concrete = concretizer.concretize(detached_sir_s,
                                                   "windows-x86_64-bootstrap",
                                                   "x86_64-pc-windows-msvc");
            verifier.verify(concrete);

            // Repeat the detachment at the target boundary. LLVM consumes only
            // this freshly deserialized standalone SIR-C graph.
            canonical_sirc = staze::serialize_sir_c(concrete);
            backend_sir_c = staze::deserialize_sir_c(canonical_sirc);
            verifier.verify(backend_sir_c);
            if (staze::serialize_sir_c(backend_sir_c) != canonical_sirc)
                throw std::runtime_error("internal: SIR-C canonical serialization is not byte-stable after round-trip");
        }

        if (!sirs_out.empty()) write_all(sirs_out, canonical_sirs);
        if (!sirc_out.empty()) write_all(sirc_out, canonical_sirc);
        if (!combined_sir_out.empty()) {
            if (from_sirc) write_all(combined_sir_out, canonical_sirc);
            else write_all(combined_sir_out, canonical_sirs + "\n" + canonical_sirc);
        }

        if (check_only) {
            std::cout << "STZ-3 detached-SIR check passed: " << input.string() << "\n";
            std::cout << "  module            : " << backend_sir_c.module_name << "\n";
            std::cout << "  functions         : " << backend_sir_c.functions.size() << "\n";
            std::cout << "  semantic facts    : " << backend_sir_c.semantic_facts.size() << "\n";
            std::size_t blocks = 0, ops = 0, phis = 0, fault_edges = 0, effects = 0;
            std::size_t regions = 0, provenances = 0, values = 0, effect_tokens = 0;
            std::size_t one = 0, zero_or_one = 0, many = 0, fault_payload_fields = 0;
            for (const auto& f : backend_sir_c.faults) fault_payload_fields += f.payload.size();
            for (const auto& fn : backend_sir_c.functions) {
                regions += fn.regions.size(); provenances += fn.provenances.size(); values += fn.values.size();
                for (const auto& v : fn.values) {
                    if (v.cardinality == staze::SirCardinality::One) ++one;
                    else if (v.cardinality == staze::SirCardinality::ZeroOrOne) ++zero_or_one;
                    else ++many;
                }
                for (const auto& b : fn.blocks) {
                    ++blocks; ++effect_tokens;
                    for (const auto& op : b.instructions) {
                        ++ops;
                        if (op.opcode == staze::SirOpcode::Phi) ++phis;
                        fault_edges += op.fault_edges.size();
                        effects += op.effects.size();
                        if (op.effect_out) ++effect_tokens;
                    }
                }
            }
            std::cout << "  datasets/choices  : " << backend_sir_c.datasets.size() << "/" << backend_sir_c.choices.size() << "\n";
            std::cout << "  relations/pools   : " << backend_sir_c.relations.size() << "/" << backend_sir_c.pools.size() << "\n";
            std::cout << "  fault payload fields: " << fault_payload_fields << "\n";
            std::cout << "  CFG blocks        : " << blocks << "\n";
            std::cout << "  SSA operations    : " << ops << "\n";
            std::cout << "  phi operations    : " << phis << "\n";
            std::cout << "  value semantics   : " << values << " (One=" << one << ", ZeroOrOne=" << zero_or_one << ", Many=" << many << ")\n";
            std::cout << "  lifetime regions  : " << regions << "\n";
            std::cout << "  provenances       : " << provenances << "\n";
            std::cout << "  explicit fault edges: " << fault_edges << "\n";
            std::cout << "  effect annotations: " << effects << "\n";
            std::cout << "  effect SSA tokens : " << effect_tokens << "\n";
            std::size_t reg=0, stack=0, caller=0, arena=0, heap=0, alias=0, scalar_replaced=0;
            for (const auto& cf : backend_sir_c.concrete_functions) {
                for (const auto& cv : cf.values) {
                    switch (cv.storage) {
                        case staze::SirStorageClass::Register: ++reg; break;
                        case staze::SirStorageClass::Stack: ++stack; break;
                        case staze::SirStorageClass::Caller: ++caller; break;
                        case staze::SirStorageClass::PoolArena: ++arena; break;
                        case staze::SirStorageClass::Heap: ++heap; break;
                        case staze::SirStorageClass::Alias: ++alias; break;
                        case staze::SirStorageClass::Static: break;
                    }
                    if (cv.scalar_replaced) ++scalar_replaced;
                }
            }
            std::cout << "  concrete storage  : register=" << reg << ", stack=" << stack
                      << ", caller=" << caller << ", pool-arena=" << arena
                      << ", heap=" << heap << ", alias=" << alias << "\n";
            std::cout << "  scalar replacements: " << scalar_replaced << "\n";
            std::cout << "  SIR-C format      : " << backend_sir_c.format_major << '.' << backend_sir_c.format_minor << "\n";
            std::cout << "  frontend retained : NO\n";
            return 0;
        }

        staze::LlvmWindowsX64Emitter emitter;
        const auto llvm = emitter.emit(backend_sir_c);
        fs::path build_ir = llvm_out.empty()
            ? output.parent_path() / (output.stem().string() + ".staze.ll")
            : llvm_out;
        write_all(build_ir, llvm);

        staze::WindowsPeToolchain tools;
        tools.build(build_ir, output, {keep});
        if (llvm_out.empty() && !keep) { std::error_code ec; fs::remove(build_ir, ec); }

        std::cout << "Built Windows x86-64 Staze executable from standalone SIR-C\n";
        std::cout << "  input  : " << input.string() << "\n";
        std::cout << "  target : " << backend_sir_c.target_triple << "\n";
        std::cout << "  object : " << backend_sir_c.object_format << "\n";
        std::cout << "  output : " << output.string() << "\n";
        if (!sirs_out.empty()) std::cout << "  SIR-S  : " << sirs_out.string() << "\n";
        if (!sirc_out.empty()) std::cout << "  SIR-C  : " << sirc_out.string() << "\n";
        if (!combined_sir_out.empty()) std::cout << "  SIR    : " << combined_sir_out.string() << "\n";
        if (!llvm_out.empty()) std::cout << "  LLVM IR: " << llvm_out.string() << "\n";
        return 0;
    } catch (const staze::CompileError& e) {
        const auto& w = e.where();
        if (!input.empty()) std::cerr << input.string() << ':' << w.line << ':' << w.column << ": error: ";
        else std::cerr << "stazec: error: ";
        std::cerr << e.what() << "\n";
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "stazec: error: " << e.what() << "\n";
        return 1;
    }
}
