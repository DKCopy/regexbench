#include <sys/resource.h>
#include <sys/time.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

#include <boost/program_options.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include "HyperscanEngine.h"
#include "PCRE2Engine.h"
#include "PcapSource.h"
#include "RE2Engine.h"
#include "REmatchEngine.h"
#include "Rule.h"
#include "regexbench.h"

using boost::property_tree::ptree;
using boost::property_tree::read_json;
using boost::property_tree::write_json;

namespace po = boost::program_options;

enum EngineType : uint64_t {
  ENGINE_HYPERSCAN,
  ENGINE_PCRE2,
  ENGINE_PCRE2JIT,
  ENGINE_RE2,
  ENGINE_REMATCH
};

struct Arguments {
  std::string output_file;
  std::string pcap_file;
  std::string rule_file;
  EngineType engine;
  int32_t repeat;
  uint32_t pcre2_concat;
  uint32_t rematch_session;
  char paddings[4];
};

template<typename Derived, typename Base, typename Del>
std::unique_ptr<Derived, Del>
static_unique_ptr_cast( std::unique_ptr<Base, Del>&& p )
{
  auto d = static_cast<Derived *>(p.release());
  return std::unique_ptr<Derived, Del>(d, std::move(p.get_deleter()));
}

static bool endsWith(const std::string &, const char *);
static Arguments parse_options(int argc, const char *argv[]);
static void compilePCRE2(const Arguments &,
                         std::unique_ptr<regexbench::Engine> &);

int main(int argc, const char *argv[]) {
  try {
    auto args = parse_options(argc, argv);
    std::unique_ptr<regexbench::Engine> engine;
    switch (args.engine) {
    case ENGINE_HYPERSCAN:
      engine = std::make_unique<regexbench::HyperscanEngine>();
      engine->compile(regexbench::loadRules(args.rule_file));
      break;
    case ENGINE_PCRE2:
      engine = std::make_unique<regexbench::PCRE2Engine>();
      compilePCRE2(args, engine);
      break;
    case ENGINE_PCRE2JIT:
      engine = std::make_unique<regexbench::PCRE2JITEngine>();
      compilePCRE2(args, engine);
      break;
    case ENGINE_RE2:
      engine = std::make_unique<regexbench::RE2Engine>();
      engine->compile(regexbench::loadRules(args.rule_file));
      break;
    case ENGINE_REMATCH:
      if (args.rematch_session) {
        engine = std::make_unique<regexbench::REmatchAutomataEngineSession>();
        engine->compile(regexbench::loadRules(args.rule_file));
      } else if (endsWith(args.rule_file, ".nfa")) {
        engine = std::make_unique<regexbench::REmatchAutomataEngine>();
        engine->load(args.rule_file);
      } else if (endsWith(args.rule_file, ".so")) {
        engine = std::make_unique<regexbench::REmatchSOEngine>();
        engine->load(args.rule_file);
      } else {
        engine = std::make_unique<regexbench::REmatchAutomataEngine>();
        engine->compile(regexbench::loadRules(args.rule_file));
      }
      break;
    }

    std::string reportFields[]{
        "TotalMatches", "TotalMatchedPackets",  "UserTime",     "SystemTime",
        "TotalTime",    "TotalBytes",           "TotalPackets", "Mbps",
        "Mpps",         "MaximumMemoryUsed(kB)"};
    size_t nsessions = 0;
    std::string prefix = "regexbench.";
    regexbench::PcapSource pcap(args.pcap_file);
    auto match_info = buildMatchMeta(pcap, nsessions);
    engine->init(nsessions);

    regexbench::MatchResult result;
    if (args.engine == ENGINE_REMATCH && args.rematch_session) {
      result = sessionMatch(*engine, pcap, args.repeat, match_info);
    } else {
      result = match(*engine, pcap, args.repeat, match_info);
    }
    boost::property_tree::ptree pt;
    pt.put(prefix + "TotalMatches", result.nmatches);
    pt.put(prefix + "TotalMatchedPackets", result.nmatched_pkts);
    std::stringstream ss;
    ss << result.udiff.tv_sec << "." << result.udiff.tv_usec;
    pt.put(prefix + "UserTime", ss.str());
    ss.str("");
    ss << result.sdiff.tv_sec << "." << result.sdiff.tv_usec;
    pt.put(prefix + "SystemTime", ss.str());
    ss.str("");
    struct timeval total;
    timeradd(&result.udiff, &result.sdiff, &total);
    ss << total.tv_sec << "." << total.tv_usec;
    pt.put(prefix + "TotalTime", ss.str());
    pt.put(prefix + "TotalBytes", pcap.getNumberOfBytes());
    pt.put(prefix + "TotalPackets", pcap.getNumberOfPackets());
    ss.str("");
    ss << boost::format("%1$.6f") %
              (static_cast<double>(pcap.getNumberOfBytes() *
                                   static_cast<unsigned long>(args.repeat)) /
               (total.tv_sec + total.tv_usec * 1e-6) / 1000000 * 8);
    pt.put(prefix + "Mbps", ss.str());

    ss.str("");
    ss << boost::format("%1$.6f") %
              (static_cast<double>(pcap.getNumberOfPackets() *
                                   static_cast<unsigned long>(args.repeat)) /
               (total.tv_sec + total.tv_usec * 1e-6) / 1000000);
    pt.put(prefix + "Mpps", ss.str());
    struct rusage stat;
    getrusage(RUSAGE_SELF, &stat);
    pt.put(prefix + "MaximumMemoryUsed(kB)", stat.ru_maxrss / 1000);

    std::ostringstream buf;
    write_json(buf, pt, false);
    std::ofstream outputFile(args.output_file);
    outputFile << buf.str();

    for (const auto &it : reportFields) {
      std::cout << it << " : " << pt.get<std::string>(prefix + it) << "\n";
    }
  } catch (const std::exception &e) {
    std::cerr << e.what() << std::endl;
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}

bool endsWith(const std::string &obj, const char *end) {
  auto r = obj.rfind(end);
  if ((r != std::string::npos) && (r == obj.size() - std::strlen(end)))
    return true;
  return false;
}

Arguments parse_options(int argc, const char *argv[]) {
  Arguments args;
  std::string engine;

  po::options_description posargs;
  posargs.add_options()("rule_file", po::value<std::string>(&args.rule_file),
                        "Rule (regular expression) file name");
  posargs.add_options()("pcap_file", po::value<std::string>(&args.pcap_file),
                        "pcap file name");
  po::positional_options_description positions;
  positions.add("rule_file", 1).add("pcap_file", 1);

  po::options_description optargs("Options");
  optargs.add_options()("help,h", "Print usage information.");
  optargs.add_options()(
      "engine,e", po::value<std::string>(&engine)->default_value("hyperscan"),
      "Matching engine to run.");
  optargs.add_options()("repeat,r",
                        po::value<int32_t>(&args.repeat)->default_value(1),
                        "Repeat pcap multiple times.");
  optargs.add_options()(
    "concat,c",
    po::value<uint32_t>(&args.pcre2_concat)->default_value(0),
    "Concatenate PCRE2 rules.");
  optargs.add_options()(
    "session,s",
    po::value<uint32_t>(&args.rematch_session)->default_value(0),
    "Rematch session mode.");
  optargs.add_options()(
      "output,o",
      po::value<std::string>(&args.output_file)->default_value("output.json"),
      "Output JSON file.");
  po::options_description cliargs;
  cliargs.add(posargs).add(optargs);
  po::variables_map vm;
  po::store(po::command_line_parser(argc, argv)
                .options(cliargs)
                .positional(positions)
                .run(),
            vm);
  po::notify(vm);

  if (vm.count("help")) {
    std::cout << "Usage: regexbench <rule_file> <pcap_file>" << std::endl;
    std::cout << posargs << "\n" << optargs << "\n";
    std::exit(EXIT_SUCCESS);
  }
  if (engine == "hyperscan")
    args.engine = ENGINE_HYPERSCAN;
  else if (engine == "pcre2")
    args.engine = ENGINE_PCRE2;
  else if (engine == "pcre2jit")
    args.engine = ENGINE_PCRE2JIT;
  else if (engine == "re2")
    args.engine = ENGINE_RE2;
  else if (engine == "rematch")
    args.engine = ENGINE_REMATCH;
  else {
    std::cerr << "unknown engine: " << engine << std::endl;
    std::exit(EXIT_FAILURE);
  }
  if (args.repeat <= 0) {
    std::cerr << "invalid repeat value: " << args.repeat << std::endl;
    std::exit(EXIT_FAILURE);
  }

  if (!vm.count("rule_file")) {
    std::cerr << "error: no rule file" << std::endl;
    std::exit(EXIT_FAILURE);
  }
  if (!vm.count("pcap_file")) {
    std::cerr << "error: no pcap file" << std::endl;
    std::exit(EXIT_FAILURE);
  }
  return args;
}

static void compilePCRE2(const Arguments &args,
                         std::unique_ptr<regexbench::Engine> &engine) {
  if (!args.pcre2_concat)
    engine->compile(regexbench::loadRules(args.rule_file));
  else {
    auto rules = regexbench::loadRules(args.rule_file);
    concatRules(rules);
    engine->compile(rules);
  }
}
