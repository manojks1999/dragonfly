// Copyright 2022, DragonflyDB authors.  All rights reserved.
// See LICENSE for licensing terms.
//
#include "server/debugcmd.h"

extern "C" {
#include "redis/redis_aux.h"
}

#include <absl/cleanup/cleanup.h>
#include <absl/random/random.h>
#include <absl/strings/str_cat.h>
#include <zstd.h>

#include <filesystem>

#include "base/flags.h"
#include "base/logging.h"
#include "core/string_map.h"
#include "server/blocking_controller.h"
#include "server/container_utils.h"
#include "server/engine_shard_set.h"
#include "server/error.h"
#include "server/main_service.h"
#include "server/multi_command_squasher.h"
#include "server/rdb_load.h"
#include "server/server_state.h"
#include "server/string_family.h"
#include "server/transaction.h"

using namespace std;

ABSL_DECLARE_FLAG(string, dir);
ABSL_DECLARE_FLAG(string, dbfilename);
ABSL_DECLARE_FLAG(bool, df_snapshot_format);

namespace dfly {

using namespace util;
using boost::intrusive_ptr;
using namespace facade;
namespace fs = std::filesystem;
using absl::GetFlag;
using absl::StrAppend;
using absl::StrCat;

namespace {
struct PopulateBatch {
  DbIndex dbid;
  uint64_t index[32];
  uint64_t sz = 0;

  PopulateBatch(DbIndex id) : dbid(id) {
  }
};

struct ObjInfo {
  unsigned encoding;
  unsigned bucket_id = 0;
  unsigned slot_id = 0;

  enum LockStatus { NONE, S, X } lock_status = NONE;

  int64_t ttl = INT64_MAX;
  optional<uint32_t> external_len;

  bool has_sec_precision = false;
  bool found = false;
};

struct ValueCompressInfo {
  size_t raw_size = 0;
  size_t compressed_size = 0;
};

std::string GenerateValue(size_t val_size, bool random_value, absl::InsecureBitGen* gen) {
  if (random_value) {
    return GetRandomHex(*gen, val_size);
  } else {
    return string(val_size, 'x');
  }
}

tuple<const CommandId*, absl::InlinedVector<string, 5>> GeneratePopulateCommand(
    string_view type, std::string key, size_t val_size, bool random_value, uint32_t elements,
    const CommandRegistry& registry, absl::InsecureBitGen* gen) {
  absl::InlinedVector<string, 5> args;
  args.push_back(std::move(key));

  const CommandId* cid = nullptr;
  if (type == "STRING") {
    cid = registry.Find("SET");
    args.push_back(GenerateValue(val_size, random_value, gen));
  } else if (type == "LIST") {
    cid = registry.Find("LPUSH");
    for (uint32_t i = 0; i < elements; ++i) {
      args.push_back(GenerateValue(val_size, random_value, gen));
    }
  } else if (type == "SET") {
    cid = registry.Find("SADD");
    for (size_t i = 0; i < elements; ++i) {
      args.push_back(GenerateValue(val_size, random_value, gen));
    }
  } else if (type == "HASH") {
    cid = registry.Find("HSET");
    for (size_t i = 0; i < elements; ++i) {
      args.push_back(GenerateValue(val_size / 2, random_value, gen));
      args.push_back(GenerateValue(val_size / 2, random_value, gen));
    }
  } else if (type == "ZSET") {
    cid = registry.Find("ZADD");
    for (size_t i = 0; i < elements; ++i) {
      args.push_back(absl::StrCat((*gen)() % val_size));
      args.push_back(GenerateValue(val_size, random_value, gen));
    }
  } else if (type == "JSON") {
    cid = registry.Find("JSON.SET");
    args.push_back("$");

    string json = "{";
    for (size_t i = 0; i < elements; ++i) {
      absl::StrAppend(&json, "\"", i, "\":\"", GenerateValue(val_size, random_value, gen), "\",");
    }
    json[json.size() - 1] = '}';  // Replace last ',' with '}'
    args.push_back(json);
  }
  return {cid, args};
}

void DoPopulateBatch(string_view type, string_view prefix, size_t val_size, bool random_value,
                     int32_t elements, const PopulateBatch& batch, ServerFamily* sf,
                     ConnectionContext* cntx) {
  boost::intrusive_ptr<Transaction> local_tx =
      new Transaction{sf->service().mutable_registry()->Find("EXEC")};
  local_tx->StartMultiNonAtomic();
  boost::intrusive_ptr<Transaction> stub_tx =
      new Transaction{local_tx.get(), EngineShard::tlocal()->shard_id(), nullopt};
  absl::InlinedVector<MutableSlice, 5> args_view;
  facade::CapturingReplyBuilder crb;
  ConnectionContext local_cntx{cntx, stub_tx.get(), &crb};

  absl::InsecureBitGen gen;
  for (unsigned i = 0; i < batch.sz; ++i) {
    string key = absl::StrCat(prefix, ":", batch.index[i]);

    auto [cid, args] = GeneratePopulateCommand(type, std::move(key), val_size, random_value,
                                               elements, *sf->service().mutable_registry(), &gen);
    if (!cid) {
      LOG_EVERY_N(WARNING, 10'000) << "Unable to find command, was it renamed?";
      break;
    }

    args_view.clear();
    for (auto& arg : args) {
      args_view.push_back(absl::MakeSpan(arg));
    }
    auto args_span = absl::MakeSpan(args_view);

    stub_tx->MultiSwitchCmd(cid);
    local_cntx.cid = cid;
    crb.SetReplyMode(ReplyMode::NONE);
    stub_tx->InitByArgs(local_cntx.conn_state.db_index, args_span);

    sf->service().InvokeCmd(cid, args_span, &local_cntx);
  }

  local_cntx.Inject(nullptr);
  local_tx->UnlockMulti();
}

struct ObjHist {
  base::Histogram key_len;
  base::Histogram val_len;    // overall size for the value.
  base::Histogram card;       // for sets, hashmaps etc - it's number of entries.
  base::Histogram entry_len;  // for sets, hashmaps etc - it's the length of each entry.
};

// Returns number of O(1) steps executed.
unsigned AddObjHist(PrimeIterator it, ObjHist* hist) {
  using namespace container_utils;
  const PrimeValue& pv = it->second;
  size_t val_len = 0;
  unsigned steps = 1;

  auto per_entry_cb = [&](ContainerEntry entry) {
    if (entry.value) {
      val_len += entry.length;
      hist->entry_len.Add(entry.length);
    } else {
      val_len += 8;  // size of long
    }
    ++steps;
    return true;
  };

  hist->key_len.Add(it->first.Size());

  if (pv.ObjType() == OBJ_LIST) {
    IterateList(pv, per_entry_cb, 0, -1);
  } else if (pv.ObjType() == OBJ_ZSET) {
    IterateSortedSet(pv.GetRobjWrapper(),
                     [&](ContainerEntry entry, double) { return per_entry_cb(entry); });
  } else if (pv.ObjType() == OBJ_SET) {
    IterateSet(pv, per_entry_cb);
  } else if (pv.ObjType() == OBJ_HASH) {
    if (pv.Encoding() == kEncodingListPack) {
      uint8_t intbuf[LP_INTBUF_SIZE];
      uint8_t* lp = (uint8_t*)pv.RObjPtr();
      uint8_t* fptr = lpFirst(lp);
      while (fptr) {
        size_t entry_len = 0;
        // field
        string_view sv = LpGetView(fptr, intbuf);
        entry_len += sv.size();

        // value
        fptr = lpNext(lp, fptr);
        entry_len += sv.size();
        fptr = lpNext(lp, fptr);
        hist->entry_len.Add(entry_len);
        steps += 2;
      }
      val_len = lpBytes(lp);
    } else {
      StringMap* sm = static_cast<StringMap*>(pv.RObjPtr());
      for (const auto& k_v : *sm) {
        hist->entry_len.Add(sdslen(k_v.first) + sdslen(k_v.second) + 2);
        ++steps;
      }
      val_len = sm->ObjMallocUsed() + sm->SetMallocUsed();
    }
  }
  // TODO: streams

  if (val_len == 0) {
    // Fallback
    val_len = pv.MallocUsed();
  }

  hist->val_len.Add(val_len);

  if (pv.ObjType() != OBJ_STRING && pv.ObjType() != OBJ_JSON)
    hist->card.Add(pv.Size());

  return steps;
}

using ObjHistMap = absl::flat_hash_map<unsigned, unique_ptr<ObjHist>>;

void MergeObjHistMap(ObjHistMap&& src, ObjHistMap* dest) {
  for (auto& [obj_type, src_hist] : src) {
    auto& dest_hist = (*dest)[obj_type];
    if (!dest_hist) {
      dest_hist = std::move(src_hist);
    } else {
      dest_hist->key_len.Merge(src_hist->key_len);
      dest_hist->val_len.Merge(src_hist->val_len);
      dest_hist->card.Merge(src_hist->card);
      dest_hist->entry_len.Merge(src_hist->entry_len);
    }
  }
}

void DoBuildObjHist(EngineShard* shard, ObjHistMap* obj_hist_map) {
  auto& db_slice = shard->db_slice();
  unsigned steps = 0;

  for (unsigned i = 0; i < db_slice.db_array_size(); ++i) {
    DbTable* dbt = db_slice.GetDBTable(i);
    if (dbt == nullptr)
      continue;
    PrimeTable::Cursor cursor;
    do {
      cursor = dbt->prime.Traverse(cursor, [&](PrimeIterator it) {
        unsigned obj_type = it->second.ObjType();
        auto& hist_ptr = (*obj_hist_map)[obj_type];
        if (!hist_ptr) {
          hist_ptr.reset(new ObjHist);
        }
        steps += AddObjHist(it, hist_ptr.get());
      });

      if (steps >= 20000) {
        steps = 0;
        ThisFiber::Yield();
      }
    } while (cursor);
  }
}

ObjInfo InspectOp(string_view key, DbIndex db_index) {
  auto& db_slice = EngineShard::tlocal()->db_slice();
  auto [pt, exp_t] = db_slice.GetTables(db_index);

  PrimeIterator it = pt->Find(key);
  ObjInfo oinfo;
  if (IsValid(it)) {
    const PrimeValue& pv = it->second;

    oinfo.found = true;
    oinfo.encoding = pv.Encoding();
    oinfo.bucket_id = it.bucket_id();
    oinfo.slot_id = it.slot_id();
    if (pv.IsExternal()) {
      oinfo.external_len.emplace(pv.GetExternalSlice().second);
    }

    if (pv.HasExpire()) {
      ExpireIterator exp_it = exp_t->Find(it->first);
      CHECK(!exp_it.is_done());

      time_t exp_time = db_slice.ExpireTime(exp_it);
      oinfo.ttl = exp_time - GetCurrentTimeMs();
      oinfo.has_sec_precision = exp_it->second.is_second_precision();
    }
  }

  KeyLockArgs lock_args;
  lock_args.args = ArgSlice{&key, 1};
  lock_args.key_step = 1;
  lock_args.db_index = db_index;

  if (!db_slice.CheckLock(IntentLock::EXCLUSIVE, lock_args)) {
    oinfo.lock_status = db_slice.CheckLock(IntentLock::SHARED, lock_args) ? ObjInfo::S : ObjInfo::X;
  }

  return oinfo;
}

OpResult<ValueCompressInfo> EstimateCompression(string_view key, DbIndex db_index) {
  auto& db_slice = EngineShard::tlocal()->db_slice();
  auto [pt, exp_t] = db_slice.GetTables(db_index);

  PrimeIterator it = pt->Find(key);
  if (!IsValid(it)) {
    return OpStatus::KEY_NOTFOUND;
  }

  // Only strings are supported right now.
  if (it->second.ObjType() != OBJ_STRING) {
    return OpStatus::WRONG_TYPE;
  }
  string scratch;
  string_view value = it->second.GetSlice(&scratch);

  ValueCompressInfo info;
  info.raw_size = value.size();
  info.compressed_size = info.raw_size;

  if (info.raw_size >= 32) {
    size_t compressed_size = ZSTD_compressBound(value.size());
    unique_ptr<char[]> compressed(new char[compressed_size]);
    info.compressed_size =
        ZSTD_compress(compressed.get(), compressed_size, value.data(), value.size(), 5);
  }

  return info;
};

}  // namespace

DebugCmd::DebugCmd(ServerFamily* owner, ConnectionContext* cntx) : sf_(*owner), cntx_(cntx) {
}

void DebugCmd::Run(CmdArgList args) {
  string_view subcmd = ArgS(args, 0);
  if (subcmd == "HELP") {
    string_view help_arr[] = {
        "DEBUG <subcommand> [<arg> [value] [opt] ...]. Subcommands are:",
        "EXEC",
        "    Show the descriptors of the MULTI/EXEC transactions that were processed by ",
        "    the server. For each EXEC/i descriptor, 'i' is the number of shards it touches. ",
        "    Each descriptor details the commands it contained followed by number of their ",
        "    arguments. Each descriptor is prefixed by its frequency count",
        "OBJECT <key> [COMPRESS]",
        "    Show low-level info about `key` and associated value.",
        "LOAD <filename>",
        "RELOAD [option ...]",
        "    Save the RDB on disk and reload it back to memory. Valid <option> values:",
        "    * NOSAVE: the database will be loaded from an existing RDB file.",
        "    Examples:",
        "    * DEBUG RELOAD NOSAVE: replace the current database with the contents of an",
        "      existing RDB file.",
        "REPLICA PAUSE/RESUME",
        "    Stops replica from reconnecting to master, or resumes",
        "REPLICA OFFSET",
        "    Return sync id and array of number of journal commands executed for each replica flow",
        "WATCHED",
        "    Shows the watched keys as a result of BLPOP and similar operations.",
        "POPULATE <count> [prefix] [size] [RAND] [SLOTS start end] [TYPE type] [ELEMENTS elements]",
        "    Create <count> string keys named key:<num> with value value:<num>.",
        "    If <prefix> is specified then it is used instead of the 'key' prefix.",
        "    If <size> is specified then X character is concatenated multiple times to value:<num>",
        "    to meet value size.",
        "    If RAND is specified then value will be set to random hex string in specified size.",
        "    If SLOTS is specified then create keys only in given slots range.",
        "    TYPE specifies data type (must be STRING/LIST/SET/HSET/ZSET/JSON), default STRING.",
        "    ELEMENTS specifies how many sub elements if relevant (like entries in a list / set).",
        "OBJHIST",
        "    Prints histogram of object sizes.",
        "STACKTRACE",
        "    Prints the stacktraces of all current fibers to the logs.",
        "SHARDS",
        "    Prints memory usage and key stats per shard, as well as min/max indicators.",
        "TX",
        "    Performs transaction analysis per shard.",
        "TRAFFIC <path> | [STOP]"
        "    Starts traffic logging to the specified path. If path is not specified,"
        "    traffic logging is stopped.",
        "HELP",
        "    Prints this help.",
    };
    auto* rb = static_cast<RedisReplyBuilder*>(cntx_->reply_builder());
    return rb->SendSimpleStrArr(help_arr);
  }

  VLOG(1) << "subcmd " << subcmd;

  if (subcmd == "POPULATE") {
    return Populate(args);
  }

  if (subcmd == "RELOAD") {
    return Reload(args);
  }

  if (subcmd == "REPLICA" && args.size() == 2) {
    return Replica(args);
  }

  if (subcmd == "WATCHED") {
    return Watched();
  }

  if (subcmd == "LOAD" && args.size() == 2) {
    return Load(ArgS(args, 1));
  }

  if (subcmd == "OBJECT" && args.size() >= 2) {
    string_view key = ArgS(args, 1);
    args.remove_prefix(2);
    return Inspect(key, args);
  }

  if (subcmd == "TX") {
    return TxAnalysis();
  }

  if (subcmd == "OBJHIST") {
    return ObjHist();
  }

  if (subcmd == "STACKTRACE") {
    return Stacktrace();
  }

  if (subcmd == "SHARDS") {
    return Shards();
  }

  if (subcmd == "EXEC") {
    return Exec();
  }

  if (subcmd == "TRAFFIC") {
    return LogTraffic(args.subspan(1));
  }

  string reply = UnknownSubCmd(subcmd, "DEBUG");
  return cntx_->SendError(reply, kSyntaxErrType);
}

void DebugCmd::Shutdown() {
  // disable traffic logging
  shard_set->pool()->AwaitFiberOnAll([](auto*) { facade::Connection::StopTrafficLogging(); });
}

void DebugCmd::Reload(CmdArgList args) {
  bool save = true;

  for (size_t i = 1; i < args.size(); ++i) {
    ToUpper(&args[i]);
    string_view opt = ArgS(args, i);
    VLOG(1) << "opt " << opt;

    if (opt == "NOSAVE") {
      save = false;
    } else {
      return cntx_->SendError("DEBUG RELOAD only supports the NOSAVE options.");
    }
  }

  if (save) {
    string err_details;
    VLOG(1) << "Performing save";

    GenericError ec = sf_.DoSave();
    if (ec) {
      return cntx_->SendError(ec.Format());
    }
  }

  string last_save_file = sf_.GetLastSaveInfo().file_name;
  Load(last_save_file);
}

void DebugCmd::Replica(CmdArgList args) {
  args.remove_prefix(1);
  ToUpper(&args[0]);
  string_view opt = ArgS(args, 0);

  auto* rb = static_cast<RedisReplyBuilder*>(cntx_->reply_builder());
  if (opt == "PAUSE" || opt == "RESUME") {
    sf_.PauseReplication(opt == "PAUSE");
    return rb->SendOk();
  } else if (opt == "OFFSET") {
    const auto offset_info = sf_.GetReplicaOffsetInfo();
    if (offset_info) {
      rb->StartArray(2);
      rb->SendBulkString(offset_info.value().sync_id);
      rb->StartArray(offset_info.value().flow_offsets.size());
      for (uint64_t offset : offset_info.value().flow_offsets) {
        rb->SendLong(offset);
      }
      return;
    } else {
      return rb->SendError("I am master");
    }
  }
  return rb->SendError(UnknownSubCmd("replica", "DEBUG"));
}

void DebugCmd::Load(string_view filename) {
  auto new_state = sf_.service().SwitchState(GlobalState::ACTIVE, GlobalState::LOADING);
  if (new_state.first != GlobalState::LOADING) {
    LOG(WARNING) << GlobalStateName(new_state.first) << " in progress, ignored";
    return;
  }

  absl::Cleanup rev_state = [this] {
    sf_.service().SwitchState(GlobalState::LOADING, GlobalState::ACTIVE);
  };

  const CommandId* cid = sf_.service().FindCmd("FLUSHALL");
  intrusive_ptr<Transaction> flush_trans(new Transaction{cid});
  flush_trans->InitByArgs(0, {});
  VLOG(1) << "Performing flush";
  error_code ec = sf_.Drakarys(flush_trans.get(), DbSlice::kDbAll);
  if (ec) {
    LOG(ERROR) << "Error flushing db " << ec.message();
  }

  fs::path path(filename);

  if (filename.empty()) {
    fs::path dir_path(GetFlag(FLAGS_dir));
    string filename = GetFlag(FLAGS_dbfilename);
    dir_path.append(filename);
    path = dir_path;
  }

  auto fut_ec = sf_.Load(path.generic_string());
  if (fut_ec.valid()) {
    ec = fut_ec.get();
    if (ec) {
      LOG(INFO) << "Could not load file " << ec.message();
      return cntx_->SendError(ec.message());
    }
  }

  cntx_->SendOk();
}

optional<DebugCmd::PopulateOptions> DebugCmd::ParsePopulateArgs(CmdArgList args) {
  if (args.size() < 2) {
    cntx_->SendError(UnknownSubCmd("populate", "DEBUG"));
    return nullopt;
  }

  PopulateOptions options;
  if (!absl::SimpleAtoi(ArgS(args, 1), &options.total_count)) {
    cntx_->SendError(kUintErr);
    return nullopt;
  }

  if (args.size() > 2) {
    options.prefix = ArgS(args, 2);
  }

  if (args.size() > 3) {
    if (!absl::SimpleAtoi(ArgS(args, 3), &options.val_size)) {
      cntx_->SendError(kUintErr);
      return nullopt;
    }
  }

  for (size_t index = 4; args.size() > index; ++index) {
    ToUpper(&args[index]);
    std::string_view str = ArgS(args, index);
    if (str == "RAND") {
      options.populate_random_values = true;
    } else if (str == "TYPE") {
      if (args.size() < index + 2) {
        cntx_->SendError(kSyntaxErr);
        return nullopt;
      }
      ToUpper(&args[++index]);
      options.type = ArgS(args, index);
    } else if (str == "ELEMENTS") {
      if (args.size() < index + 2) {
        cntx_->SendError(kSyntaxErr);
        return nullopt;
      }
      if (!absl::SimpleAtoi(ArgS(args, ++index), &options.elements)) {
        cntx_->SendError(kSyntaxErr);
        return nullopt;
      }
    } else if (str == "SLOTS") {
      if (args.size() < index + 3) {
        cntx_->SendError(kSyntaxErr);
        return nullopt;
      }

      auto parse_slot = [](string_view slot_str) -> OpResult<uint32_t> {
        uint32_t slot_id;
        if (!absl::SimpleAtoi(slot_str, &slot_id)) {
          return facade::OpStatus::INVALID_INT;
        }
        if (slot_id > ClusterConfig::kMaxSlotNum) {
          return facade::OpStatus::INVALID_VALUE;
        }
        return slot_id;
      };

      auto start = parse_slot(ArgS(args, ++index));
      if (start.status() != facade::OpStatus::OK) {
        cntx_->SendError(start.status());
        return nullopt;
      }
      auto end = parse_slot(ArgS(args, ++index));
      if (end.status() != facade::OpStatus::OK) {
        cntx_->SendError(end.status());
        return nullopt;
      }
      options.slot_range = SlotRange{.start = static_cast<SlotId>(start.value()),
                                     .end = static_cast<SlotId>(end.value())};

    } else {
      cntx_->SendError(kSyntaxErr);
      return nullopt;
    }
  }
  return options;
}

void DebugCmd::Populate(CmdArgList args) {
  optional<PopulateOptions> options = ParsePopulateArgs(args);
  if (!options.has_value()) {
    return;
  }
  ProactorPool& pp = sf_.service().proactor_pool();
  size_t runners_count = pp.size();
  vector<pair<uint64_t, uint64_t>> ranges(runners_count - 1);
  uint64_t batch_size = options->total_count / runners_count;
  size_t from = 0;
  for (size_t i = 0; i < ranges.size(); ++i) {
    ranges[i].first = from;
    ranges[i].second = batch_size;
    from += batch_size;
  }
  ranges.emplace_back(from, options->total_count - from);

  vector<Fiber> fb_arr(ranges.size());
  for (size_t i = 0; i < ranges.size(); ++i) {
    auto range = ranges[i];

    // whatever we do, we should not capture i by reference.
    fb_arr[i] = pp.at(i)->LaunchFiber([range, options, this] {
      this->PopulateRangeFiber(range.first, range.second, options.value());
    });
  }
  for (auto& fb : fb_arr)
    fb.Join();

  cntx_->SendOk();
}

void DebugCmd::PopulateRangeFiber(uint64_t from, uint64_t num_of_keys,
                                  const PopulateOptions& options) {
  ThisFiber::SetName("populate_range");
  VLOG(1) << "PopulateRange: " << from << "-" << (from + num_of_keys - 1);

  string key = absl::StrCat(options.prefix, ":");
  size_t prefsize = key.size();
  DbIndex db_indx = cntx_->db_index();
  EngineShardSet& ess = *shard_set;
  std::vector<PopulateBatch> ps(ess.size(), PopulateBatch{db_indx});

  uint64_t index = from;
  uint64_t to = from + num_of_keys;
  uint64_t added = 0;
  while (added < num_of_keys) {
    if ((index >= to) && ((index - to) % options.total_count == 0)) {
      index = index - num_of_keys + options.total_count;
    }
    key.resize(prefsize);  // shrink back

    StrAppend(&key, index);

    if (options.slot_range.has_value()) {
      // Each fiber will add num_of_keys. Keys are in the form of <key_prefix>:<index>
      // We need to make sure that different fibers will not add the same key.
      // Fiber starting <key_prefix>:<from> to <key_prefix>:<from+num_of_keys-1>
      // then continue to <key_prefix>:<from+total_count> to
      // <key_prefix>:<from+total_count+num_of_keys-1> and continue until num_of_keys are added.

      // Add keys only in slot range.
      SlotId sid = ClusterConfig::KeySlot(key);
      if (sid < options.slot_range->start || sid > options.slot_range->end) {
        ++index;
        continue;
      }
    }
    ShardId sid = Shard(key, ess.size());

    auto& shard_batch = ps[sid];
    shard_batch.index[shard_batch.sz++] = index;
    ++added;
    ++index;

    if (shard_batch.sz == 32) {
      ess.Add(sid, [this, index, options, shard_batch] {
        DoPopulateBatch(options.type, options.prefix, options.val_size,
                        options.populate_random_values, options.elements, shard_batch, &sf_, cntx_);
        if (index % 50 == 0) {
          ThisFiber::Yield();
        }
      });

      // we capture shard_batch by value so we can override it here.
      shard_batch.sz = 0;
    }
  }

  ess.AwaitRunningOnShardQueue([&](EngineShard* shard) {
    DoPopulateBatch(options.type, options.prefix, options.val_size, options.populate_random_values,
                    options.elements, ps[shard->shard_id()], &sf_, cntx_);
    // Debug populate does not use transaction framework therefore we call OnCbFinish manually
    // after running the callback
    // Note that running debug populate while running flushall/db can cause dcheck fail because the
    // finish cb is executed just when we finish populating the database.
    shard->db_slice().OnCbFinish();
  });
}

void DebugCmd::Exec() {
  EngineShardSet& ess = *shard_set;
  fb2::Mutex mu;
  std::map<string, unsigned> freq_cnt;

  ess.pool()->AwaitFiberOnAll([&](auto*) {
    for (const auto& k_v : ServerState::tlocal()->exec_freq_count) {
      unique_lock lk(mu);
      freq_cnt[k_v.first] += k_v.second;
    }
  });

  string res;
  for (const auto& k_v : freq_cnt) {
    StrAppend(&res, k_v.second, ":", k_v.first, "\n");
  }
  auto* rb = static_cast<RedisReplyBuilder*>(cntx_->reply_builder());
  rb->SendVerbatimString(res);
}

void DebugCmd::LogTraffic(CmdArgList args) {
  optional<string> path;
  if (args.size() == 1 && absl::AsciiStrToUpper(facade::ToSV(args.front())) != "STOP"sv) {
    path = ArgS(args, 0);
    LOG(INFO) << "Logging to traffic to " << *path << "*.bin";
  } else {
    LOG(INFO) << "Traffic logging stopped";
  }

  shard_set->pool()->AwaitFiberOnAll([path](auto*) {
    if (path)
      facade::Connection::StartTrafficLogging(*path);
    else
      facade::Connection::StopTrafficLogging();
  });
  cntx_->SendOk();
}

void DebugCmd::Inspect(string_view key, CmdArgList args) {
  EngineShardSet& ess = *shard_set;
  ShardId sid = Shard(key, ess.size());
  VLOG(1) << "DebugCmd::Inspect " << key;

  bool check_compression = (args.size() == 1) && ArgS(args, 0) == "COMPRESS";
  string resp;

  if (check_compression) {
    auto cb = [&] { return EstimateCompression(key, cntx_->db_index()); };
    auto res = ess.Await(sid, std::move(cb));
    if (!res) {
      cntx_->SendError(res.status());
      return;
    }
    StrAppend(&resp, "raw_size: ", res->raw_size, ", compressed_size: ", res->compressed_size);
    if (res->raw_size > 0) {
      StrAppend(&resp, " ratio: ", static_cast<double>(res->compressed_size) / (res->raw_size));
    }
  } else {
    auto cb = [&] { return InspectOp(key, cntx_->db_index()); };

    ObjInfo res = ess.Await(sid, std::move(cb));

    if (!res.found) {
      cntx_->SendError(kKeyNotFoundErr);
      return;
    }

    StrAppend(&resp, "encoding:", strEncoding(res.encoding), " bucket_id:", res.bucket_id);
    StrAppend(&resp, " slot:", res.slot_id, " shard:", sid);

    if (res.ttl != INT64_MAX) {
      StrAppend(&resp, " ttl:", res.ttl, res.has_sec_precision ? "s" : "ms");
    }

    if (res.external_len) {
      StrAppend(&resp, " spill_len:", *res.external_len);
    }

    if (res.lock_status != ObjInfo::NONE) {
      StrAppend(&resp, " lock:", res.lock_status == ObjInfo::X ? "x" : "s");
    }
  }
  cntx_->SendSimpleString(resp);
}

void DebugCmd::Watched() {
  Mutex mu;

  vector<string> watched_keys;
  vector<string> awaked_trans;

  auto cb = [&](EngineShard* shard) {
    auto* bc = shard->blocking_controller();
    if (bc) {
      auto keys = bc->GetWatchedKeys(cntx_->db_index());

      lock_guard lk(mu);
      watched_keys.insert(watched_keys.end(), keys.begin(), keys.end());
      for (auto* tx : bc->awakened_transactions()) {
        awaked_trans.push_back(StrCat("[", shard->shard_id(), "] ", tx->DebugId()));
      }
    }
  };

  auto* rb = static_cast<RedisReplyBuilder*>(cntx_->reply_builder());
  shard_set->RunBlockingInParallel(cb);
  rb->StartArray(4);
  rb->SendBulkString("awaked");
  rb->SendStringArr(awaked_trans);
  rb->SendBulkString("watched");
  rb->SendStringArr(watched_keys);
}

void DebugCmd::TxAnalysis() {
  vector<EngineShard::TxQueueInfo> shard_info(shard_set->size());

  auto cb = [&](EngineShard* shard) {
    auto& info = shard_info[shard->shard_id()];
    info = shard->AnalyzeTxQueue();
  };

  shard_set->RunBriefInParallel(cb);

  string result;
  for (unsigned i = 0; i < shard_set->size(); ++i) {
    const auto& info = shard_info[i];
    StrAppend(&result, "shard", i, ":\n", "  tx armed ", info.tx_armed, ", total: ", info.tx_total,
              ",global:", info.tx_global, ",runnable:", info.tx_runnable, "\n");
    StrAppend(&result, "  locks total:", info.total_locks, ",contended:", info.contended_locks,
              "\n");
    StrAppend(&result, "  max contention score: ", info.max_contention_score,
              ",lock_name:", info.max_contention_lock_name, "\n");
  }
  auto* rb = static_cast<RedisReplyBuilder*>(cntx_->reply_builder());
  rb->SendVerbatimString(result);
}

void DebugCmd::ObjHist() {
  vector<ObjHistMap> obj_hist_map_arr(shard_set->size());

  shard_set->RunBlockingInParallel(
      [&](EngineShard* shard) { DoBuildObjHist(shard, &obj_hist_map_arr[shard->shard_id()]); });

  for (size_t i = shard_set->size() - 1; i > 0; --i) {
    MergeObjHistMap(std::move(obj_hist_map_arr[i]), &obj_hist_map_arr[0]);
  }

  string result;
  absl::StrAppend(&result, "___begin object histogram___\n\n");

  for (auto& [obj_type, hist_ptr] : obj_hist_map_arr[0]) {
    StrAppend(&result, "OBJECT:", ObjTypeName(obj_type), "\n");
    StrAppend(&result, "________________________________________________________________\n");
    StrAppend(&result, "Key length histogram:\n", hist_ptr->key_len.ToString(), "\n");
    StrAppend(&result, "Value length histogram:\n", hist_ptr->val_len.ToString(), "\n");
    StrAppend(&result, "Cardinality histogram:\n", hist_ptr->card.ToString(), "\n");
    StrAppend(&result, "Entry length histogram:\n", hist_ptr->entry_len.ToString(), "\n");
  }

  absl::StrAppend(&result, "___end object histogram___\n");
  auto* rb = static_cast<RedisReplyBuilder*>(cntx_->reply_builder());
  rb->SendVerbatimString(result);
}

void DebugCmd::Stacktrace() {
  fb2::Mutex m;
  shard_set->pool()->AwaitFiberOnAll([&m](unsigned index, ProactorBase* base) {
    std::unique_lock lk(m);
    fb2::detail::FiberInterface::PrintAllFiberStackTraces();
  });
  cntx_->SendOk();
}

void DebugCmd::Shards() {
  struct ShardInfo {
    size_t used_memory = 0;
    size_t key_count = 0;
    size_t expire_count = 0;
    size_t key_reads = 0;
  };

  vector<ShardInfo> infos(shard_set->size());
  shard_set->RunBriefInParallel([&](EngineShard* shard) {
    auto slice_stats = shard->db_slice().GetStats();
    auto& stats = infos[shard->shard_id()];

    stats.used_memory = shard->UsedMemory();
    for (const auto& db_stats : slice_stats.db_stats) {
      stats.key_count += db_stats.key_count;
      stats.expire_count += db_stats.expire_count;
    }
    stats.key_reads = slice_stats.events.hits + slice_stats.events.misses;
  });

#define ADD_STAT(i, stat) absl::StrAppend(&out, "shard", i, "_", #stat, ": ", infos[i].stat, "\n");
#define MAXMIN_STAT(stat)                                   \
  {                                                         \
    size_t minv = numeric_limits<size_t>::max();            \
    size_t maxv = 0;                                        \
    for (const auto& info : infos) {                        \
      minv = min(minv, info.stat);                          \
      maxv = max(maxv, info.stat);                          \
    }                                                       \
    absl::StrAppend(&out, "max_", #stat, ": ", maxv, "\n"); \
    absl::StrAppend(&out, "min_", #stat, ": ", minv, "\n"); \
  }

  string out;
  absl::StrAppend(&out, "num_shards: ", shard_set->size(), "\n");

  for (size_t i = 0; i < infos.size(); i++) {
    ADD_STAT(i, used_memory);
    ADD_STAT(i, key_count);
    ADD_STAT(i, expire_count);
    ADD_STAT(i, key_reads);
  }

  MAXMIN_STAT(used_memory);
  MAXMIN_STAT(key_count);
  MAXMIN_STAT(expire_count);
  MAXMIN_STAT(key_reads);

#undef ADD_STAT
#undef MAXMIN_STAT
  auto* rb = static_cast<RedisReplyBuilder*>(cntx_->reply_builder());
  rb->SendVerbatimString(out);
}

}  // namespace dfly
