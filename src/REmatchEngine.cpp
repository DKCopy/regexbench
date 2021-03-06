#include <dlfcn.h>

#include <sys/resource.h>
#include <sys/time.h>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <tuple>

#include <dlfcn.h>

#include <rematch/compile.h>
#include <rematch/execute.h>
#include <rematch/rematch.h>

#include "REmatchEngine.h"

using namespace regexbench;

const char NFA_FUNC_NAME[] = "run";
const char NFA_NSTATES_NAME[] = "nstates";

REmatchAutomataEngine::REmatchAutomataEngine(uint32_t nm, bool red)
    : flow(nullptr), matcher(nullptr), txtbl(nullptr),
      regmatchMem(std::make_unique<mregmatch_t[]>(nm)),
      regmatch(regmatchMem.get()), nmatch((nm < 1) ? MAX_NMATCH : nm),
      reduce(red)
{
}

REmatchAutomataEngine::~REmatchAutomataEngine()
{
  if (flow)
    mregflow_delete(flow);
  if (matcher)
    matcher_delete(matcher);
  if (txtbl)
    mregfree(txtbl);
}

void REmatchAutomataEngine::compile(const std::vector<Rule>& rules, size_t)
{
  std::vector<const char*> exps;
  std::vector<unsigned> mods;
  std::vector<unsigned> ids;
  for (const auto& rule : rules) {
    exps.push_back(rule.getRegexp().data());
    ids.push_back(static_cast<unsigned>(rule.getID()));
    uint32_t opt = 0;
    if (rule.isSet(MOD_CASELESS))
      opt |= REMATCH_MOD_CASELESS;
    if (rule.isSet(MOD_MULTILINE))
      opt |= REMATCH_MOD_MULTILINE;
    if (rule.isSet(MOD_DOTALL))
      opt |= REMATCH_MOD_DOTALL;
    mods.push_back(opt);
  }
  txtbl =
      rematch_compile(ids.data(), exps.data(), mods.data(), ids.size(), reduce);
  flow = mregflow_new(txtbl->nstates, 1, 1);
  matcher = matcher_new(txtbl->nstates);
}

void REmatchAutomataEngine::load(const std::string& filename, size_t)
{
  txtbl = rematchload(filename.c_str());
  if (txtbl == nullptr) {
    throw std::runtime_error("cannot load NFA");
  }
  flow = mregflow_new(txtbl->nstates, 1, 1);
  if (flow == nullptr)
    throw std::bad_alloc();
  matcher = matcher_new(txtbl->nstates);
  if (matcher == nullptr)
    throw std::bad_alloc();
}

size_t REmatchAutomataEngine::match(const char* data, size_t len, size_t,
                                    size_t /*thr*/, size_t* /*pId*/)
{
  mregexec_single(txtbl, data, len, nmatch, regmatch, matcher, flow);
  return matcher->matches;
}

REmatchSOEngine::REmatchSOEngine()
    : run(nullptr), ctx(nullptr), dlhandle(nullptr)
{
}

REmatchSOEngine::~REmatchSOEngine()
{
  if (ctx)
    destroy_matchctx(ctx);
  if (dlhandle)
    dlclose(dlhandle);
}

void REmatchSOEngine::load(const std::string& filename, size_t)
{
  dlhandle = dlopen(filename.c_str(), RTLD_LAZY);
  if (dlhandle == nullptr) {
    char* error = dlerror();
    if (error != nullptr)
      throw std::runtime_error(error);
    throw std::runtime_error("fail to load " + filename);
  }
  size_t* p = reinterpret_cast<size_t*>(dlsym(dlhandle, NFA_NSTATES_NAME));
  if (p == nullptr) {
    char* error = dlerror();
    if (error != nullptr)
      throw std::runtime_error(error);
    throw std::runtime_error("cannot find symol");
  }
  ctx = create_matchctx(*p);
  if (ctx == nullptr)
    throw std::bad_alloc();
  run = reinterpret_cast<run_func_t>(dlsym(dlhandle, NFA_FUNC_NAME));
  if (run == nullptr) {
    char* error = dlerror();
    if (error != nullptr)
      throw std::runtime_error(error);
    throw std::runtime_error("cannot find symol");
  }
}

#ifdef WITH_SESSION
REmatchAutomataEngineSession::REmatchAutomataEngineSession(uint32_t nm)
    : REmatchAutomataEngine(nm), parent{nullptr}, child{nullptr}
{
}
REmatchAutomataEngineSession::~REmatchAutomataEngineSession()
{
  mregSession_delete_parent(parent);
  mregSession_delete_child(child);
}

size_t REmatchAutomataEngineSession::match(const char* pkt, size_t len,
                                           size_t idx, size_t /*thr*/,
                                           size_t* /*pId*/)
{
  matcher_t* cur = child->mindex[idx];
  size_t ret = 0;
  switch (mregexec_session(txtbl, pkt, len, nmatch, regmatch, cur, child)) {
  case MREG_FINISHED: // finished
    cur->num_active = 0;
    ret = cur->matches;
    break;
  case MREG_NOT_FINISHED: // not finished
    ret = 0;
    break;
  case MREG_FAILURE:
  default:
    cur->num_active = 0;
    ret = 0;
  }
  return ret;
}

void REmatchAutomataEngineSession::init(size_t nsessions)
{
  parent = mregSession_create_parent(static_cast<uint32_t>(nsessions),
                                     txtbl->nstates);
  child = mregSession_create_child(parent, unit_total);

  for (size_t i = 0; i < nsessions; i++) {
    matcher_t* cur = child->mindex[i];
    if (cur->num_active) {
      if (child->active1 < MNULL) {
        MATCHER_SESSION_SET_NEW(cur, child);
      } else {
        throw std::bad_alloc();
      }
    }
  }
}
#endif // WITH_SESSION

REmatch2AutomataEngine::REmatch2AutomataEngine(uint32_t nm, bool red, bool tur)
    : nmatch(nm), version(0), reduce(red), turbo(tur)
{
}
REmatch2AutomataEngine::~REmatch2AutomataEngine()
{
  for (auto scratch : scratches)
    rematch_free_scratch(scratch);
  for (auto context : contexts)
    rematch2ContextFree(context);
  for (auto matcher : matchers)
    rematch2Free(matcher.second);
}

void REmatch2AutomataEngine::compile(const std::vector<Rule>& rules,
                                     size_t numThr)
{
  std::vector<const char*> exps;
  std::vector<unsigned> mods;
  std::vector<unsigned> ids;
  for (const auto& rule : rules) {
    exps.push_back(rule.getRegexp().data());
    ids.push_back(static_cast<unsigned>(rule.getID()));
    uint32_t opt = 0;
    if (rule.isSet(MOD_CASELESS))
      opt |= REMATCH_MOD_CASELESS;
    if (rule.isSet(MOD_MULTILINE))
      opt |= REMATCH_MOD_MULTILINE;
    if (rule.isSet(MOD_DOTALL))
      opt |= REMATCH_MOD_DOTALL;
    mods.push_back(opt);
  }
  auto matcher = rematch2_compile_with_shortcuts(
      ids.data(), exps.data(), mods.data(), ids.size(), reduce, turbo);
  if (matcher == nullptr) {
    throw std::runtime_error("Could not build REmatch2 matcher.");
  }
  matchers[version + 1] = matcher;
  version++;
  numThreads = numThr;
  scratches.resize(numThreads, nullptr);
  contexts.resize(numThreads, nullptr);
  versions.resize(numThreads, version - 1);
}

void REmatch2AutomataEngine::compile_test(const std::vector<Rule>& rules) const
{
  std::vector<const char*> exps;
  std::vector<unsigned> mods;
  std::vector<unsigned> ids;
  for (const auto& rule : rules) {
    exps.push_back(rule.getRegexp().data());
    ids.push_back(static_cast<unsigned>(rule.getID()));
    uint32_t opt = 0;
    if (rule.isSet(MOD_CASELESS))
      opt |= REMATCH_MOD_CASELESS;
    if (rule.isSet(MOD_MULTILINE))
      opt |= REMATCH_MOD_MULTILINE;
    if (rule.isSet(MOD_DOTALL))
      opt |= REMATCH_MOD_DOTALL;
    mods.push_back(opt);
  }
  auto testMatcher = rematch2_compile_with_shortcuts(
      ids.data(), exps.data(), mods.data(), ids.size(), reduce, turbo);
  if (testMatcher == nullptr) {
    throw std::runtime_error("Could not build REmatch2 matcher.");
  }
  rematch2Free(testMatcher);
}

double
REmatch2AutomataEngine::update_test(const std::vector<Rule>& rules,
                                    const std::vector<Rule>& update_rules)
{
  std::vector<const char*> exps;
  std::vector<unsigned> mods;
  std::vector<unsigned> ids;
  auto disassemble_rule = [&exps, &mods, &ids](const Rule& r) {
    exps.push_back(r.getRegexp().data());
    ids.push_back(static_cast<unsigned>(r.getID()));
    uint32_t opt = 0;
    if (r.isSet(MOD_CASELESS))
      opt |= REMATCH_MOD_CASELESS;
    if (r.isSet(MOD_MULTILINE))
      opt |= REMATCH_MOD_MULTILINE;
    if (r.isSet(MOD_DOTALL))
      opt |= REMATCH_MOD_DOTALL;
    mods.push_back(opt);
  };
  for (const auto& rule : rules) {
    disassemble_rule(rule);
  }

  auto testMatcher = rematch2_compile_with_dynamic_update(
      ids.data(), exps.data(), mods.data(), ids.size(), &reserved_space);
  if (testMatcher == nullptr) {
    throw std::runtime_error("Could not build REmatch2 matcher.");
  }
  exps.clear();
  mods.clear();
  ids.clear();
  for (auto rule : update_rules) {
    disassemble_rule(rule);
  }

  struct rusage updateBegin, updateEnd;
  getrusage(RUSAGE_SELF, &updateBegin);
  auto update_result = rematch2_update(ids.data(), exps.data(), mods.data(),
                                       ids.size(), testMatcher);
  if (!update_result) {
    throw std::runtime_error("Could not update matcher.");
  }
  getrusage(RUSAGE_SELF, &updateEnd);
  struct timeval udiff, sdiff;
  timersub(&(updateEnd.ru_utime), &(updateBegin.ru_utime), &udiff);
  timersub(&(updateEnd.ru_stime), &(updateBegin.ru_stime), &sdiff);
  auto compileTime =
      (udiff.tv_sec + sdiff.tv_sec + (udiff.tv_usec + sdiff.tv_usec) * 1e-6);
  rematch2Free(testMatcher);
  return compileTime;
}

void REmatch2AutomataEngine::load(const std::string& file, size_t numThr)
{
  auto matcher = matchers[version + 1] = rematch2Load(file.c_str());
  version++;
  if (matcher == nullptr)
    throw std::runtime_error("Could not load REmatch2 matcher.");
  numThreads = numThr;
  scratches.resize(numThreads, nullptr);
  contexts.resize(numThreads, nullptr);
  versions.resize(numThreads, version - 1);
}

void REmatch2AutomataEngine::load_updated(const std::string& file)
{
  auto matcher = rematch2Load(file.c_str());
  if (matcher == nullptr)
    throw std::runtime_error("Could not load REmatch2 matcher.");
  matchers[version + 1] = matcher;
  version++;
  std::cout << "Rule update soon to be applied" << std::endl;
}

static int count_matches(unsigned id, unsigned long long, unsigned long long,
                         unsigned, void* ctx)
{
  std::tuple<size_t, uint32_t, unsigned int>* matchRes =
      static_cast<std::tuple<size_t, uint32_t, unsigned int>*>(ctx);
  auto& count = std::get<0>(*matchRes);
  ++count;
  if (count == 1)
    std::get<2>(*matchRes) = id;
  auto nmatch = std::get<1>(*matchRes);
  if (nmatch > 0 && count >= nmatch)
    return 1;
  return 0;
}

size_t REmatch2AutomataEngine::match(const char* pkt, size_t len, size_t,
                                     size_t thr, size_t* pId)
{
  auto scratch = scratches[thr];
  auto context = contexts[thr];
  auto& cur_version = versions[thr];
  if (__builtin_expect((version > cur_version), false)) {
    // std::cout << "Context update to be done" << std::endl;
    cur_version = version;
    //  if prev one is to be freed, then free it first
    if (scratch)
      rematch_free_scratch(scratch);
    scratch = scratches[thr] = rematch_alloc_scratch(matchers[cur_version]);
    if (context)
      rematch2ContextFree(context);
    context = contexts[thr] = rematch2ContextInit(matchers[cur_version]);
    if (context == nullptr)
      throw std::runtime_error("Could not initialize context.");
  }
  auto cur_matcher = matchers[cur_version];
  std::tuple<size_t, uint32_t, unsigned int> matchResult{
      0, nmatch, 0}; // 1st : match count
                     // 2nd : nmatch (how many times to match)
                     // 3rd : id of first match
  rematch_scan_block(cur_matcher, pkt, len, context, scratch, count_matches,
                     &matchResult);
  size_t matched = std::get<0>(matchResult);
  if (matched > 0)
    *pId = std::get<2>(matchResult);
  rematch2ContextClear(context, true);
  return matched;
}
