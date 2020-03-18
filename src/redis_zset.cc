#include "redis_zset.h"

#include <math.h>
#include <map>
#include <limits>

namespace Redis {

rocksdb::Status ZSet::GetMetadata(const Slice &ns_key, ZSetMetadata *metadata) {
  return Database::GetMetadata(kRedisZSet, ns_key, metadata);
}

rocksdb::Status ZSet::Add(const Slice &user_key, uint8_t flags, std::vector<MemberScore> *mscores, int *ret) {
  *ret = 0;

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  ZSetMetadata metadata;
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok() && !s.IsNotFound()) return s;

  int added = 0;
  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisZSet);
  batch.PutLogData(log_data.Encode());
  std::string member_key;
  for (size_t i = 0; i < mscores->size(); i++) {
    InternalKey(ns_key, (*mscores)[i].member, metadata.version).Encode(&member_key);
    if (metadata.size > 0) {
      std::string old_score_bytes;
      s = db_->Get(rocksdb::ReadOptions(), member_key, &old_score_bytes);
      if (!s.ok() && !s.IsNotFound()) return s;
      if (s.ok()) {
        double old_score = DecodeDouble(old_score_bytes.data());
        if (flags == ZSET_INCR) {
          (*mscores)[i].score += old_score;
        }
        if ((*mscores)[i].score != old_score) {
          old_score_bytes.append((*mscores)[i].member);
          std::string old_score_key;
          InternalKey(ns_key, old_score_bytes, metadata.version).Encode(&old_score_key);
          batch.Delete(score_cf_handle_, old_score_key);
          std::string new_score_bytes, new_score_key;
          PutDouble(&new_score_bytes, (*mscores)[i].score);
          batch.Put(member_key, new_score_bytes);
          new_score_bytes.append((*mscores)[i].member);
          InternalKey(ns_key, new_score_bytes, metadata.version).Encode(&new_score_key);
          batch.Put(score_cf_handle_, new_score_key, Slice());
        }
        continue;
      }
    }
    std::string score_bytes, score_key;
    PutDouble(&score_bytes, (*mscores)[i].score);
    batch.Put(member_key, score_bytes);
    score_bytes.append((*mscores)[i].member);
    InternalKey(ns_key, score_bytes, metadata.version).Encode(&score_key);
    batch.Put(score_cf_handle_, score_key, Slice());
    added++;
  }
  if (added > 0) {
    *ret = added;
    metadata.size += added;
    std::string bytes;
    metadata.Encode(&bytes);
    batch.Put(metadata_cf_handle_, ns_key, bytes);
  }
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}

rocksdb::Status ZSet::Card(const Slice &user_key, int *ret) {
  *ret = 0;

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  ZSetMetadata metadata(true);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound()? rocksdb::Status::OK():s;
  *ret = metadata.size;
  return rocksdb::Status::OK();
}

rocksdb::Status ZSet::Count(const Slice &user_key, const ZRangeSpec &spec, int *ret) {
  *ret = 0;
  return RangeByScore(user_key, spec, nullptr, ret);
}

rocksdb::Status ZSet::IncrBy(const Slice &user_key, const Slice &member, double increment, double *score) {
  int ret;
  std::vector<MemberScore> mscores;
  mscores.emplace_back(MemberScore{member.ToString(), increment});
  rocksdb::Status s = Add(user_key, ZSET_INCR, &mscores, &ret);
  if (!s.ok()) return s;
  *score = mscores[0].score;
  return rocksdb::Status::OK();
}

rocksdb::Status ZSet::Pop(const Slice &user_key, int count, bool min, std::vector<MemberScore> *mscores) {
  mscores->clear();

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  ZSetMetadata metadata(true);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound()? rocksdb::Status::OK():s;
  if (count <=0) return rocksdb::Status::OK();
  if (count > static_cast<int>(metadata.size)) count = metadata.size;

  std::string score_bytes;
  double score = min ? std::numeric_limits<double>::lowest():std::numeric_limits<double>::max();
  PutDouble(&score_bytes, score);
  std::string start_key, prefix_key;
  InternalKey(ns_key, score_bytes, metadata.version).Encode(&start_key);
  InternalKey(ns_key, "", metadata.version).Encode(&prefix_key);

  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisZSet);
  batch.PutLogData(log_data.Encode());
  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(db_);
  read_options.snapshot = ss.GetSnapShot();
  read_options.fill_cache = false;
  auto iter = db_->NewIterator(read_options, score_cf_handle_);
  iter->Seek(start_key);
  // see comment in rangebyscore()
  if (!min && (!iter->Valid() || !iter->key().starts_with(prefix_key))) {
    iter->SeekForPrev(start_key);
  }
  for (;
      iter->Valid() && iter->key().starts_with(prefix_key);
      min ? iter->Next() : iter->Prev()) {
    InternalKey ikey(iter->key());
    Slice score_key = ikey.GetSubKey();
    GetDouble(&score_key, &score);
    mscores->emplace_back(MemberScore{score_key.ToString(), score});
    std::string default_cf_key;
    InternalKey(ns_key, score_key, metadata.version).Encode(&default_cf_key);
    batch.Delete(default_cf_key);
    batch.Delete(score_cf_handle_, iter->key());
    if (mscores->size() >= static_cast<unsigned>(count)) break;
  }
  delete iter;

  if (!mscores->empty()) {
    metadata.size -= mscores->size();
    std::string bytes;
    metadata.Encode(&bytes);
    batch.Put(metadata_cf_handle_, ns_key, bytes);
  }
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}

rocksdb::Status ZSet::Range(const Slice &user_key, int start, int stop, uint8_t flags, std::vector<MemberScore>
*mscores) {
  mscores->clear();

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  bool removed = (flags & (uint8_t)ZSET_REMOVED) != 0;
  bool reversed = (flags & (uint8_t)ZSET_REVERSED) != 0;
  if (removed) LockGuard guard(storage_->GetLockManager(), ns_key);
  ZSetMetadata metadata(true);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound()? rocksdb::Status::OK():s;
  if (start < 0) start += metadata.size;
  if (stop < 0) stop += metadata.size;
  if (start < 0 || stop < 0 || start > stop) {
    return rocksdb::Status::OK();
  }

  std::string score_bytes;
  double score = !reversed ? std::numeric_limits<double>::lowest():std::numeric_limits<double>::max();
  PutDouble(&score_bytes, score);
  std::string start_key, prefix_key;
  InternalKey(ns_key, score_bytes, metadata.version).Encode(&start_key);
  InternalKey(ns_key, "", metadata.version).Encode(&prefix_key);

  int count = 0;
  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(db_);
  read_options.snapshot = ss.GetSnapShot();
  read_options.fill_cache = false;
  rocksdb::WriteBatch batch;
  auto iter = db_->NewIterator(read_options, score_cf_handle_);
  iter->Seek(start_key);
  // see comment in rangebyscore()
  if (reversed && (!iter->Valid() || !iter->key().starts_with(prefix_key))) {
    iter->SeekForPrev(start_key);
  }
  for (;
      iter->Valid() && iter->key().starts_with(prefix_key);
      !reversed ? iter->Next() : iter->Prev()) {
    InternalKey ikey(iter->key());
    Slice score_key = ikey.GetSubKey();
    GetDouble(&score_key, &score);
    if (count >= start) {
      if (removed) {
        std::string sub_key;
        InternalKey(ns_key, score_key, metadata.version).Encode(&sub_key);
        batch.Delete(sub_key);
        batch.Delete(score_cf_handle_, iter->key());
      }
      mscores->emplace_back(MemberScore{score_key.ToString(), score});
    }
    if (count++ >= stop) break;
  }
  delete iter;

  if (removed &&count > 0) {
    metadata.size -= count;
    std::string bytes;
    metadata.Encode(&bytes);
    batch.Put(metadata_cf_handle_, ns_key, bytes);
    return storage_->Write(rocksdb::WriteOptions(), &batch);
  }
  return rocksdb::Status::OK();
}

rocksdb::Status ZSet::RangeByScore(const Slice &user_key,
                                        ZRangeSpec spec,
                                        std::vector<MemberScore> *mscores,
                                        int *size) {
  if (size) *size = 0;
  if (mscores) mscores->clear();

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  if (spec.removed) LockGuard guard(storage_->GetLockManager(), ns_key);
  ZSetMetadata metadata(true);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound()? rocksdb::Status::OK():s;

  std::string start_score_bytes;
  PutDouble(&start_score_bytes, spec.reversed ? spec.max : spec.min);
  std::string start_key, prefix_key;
  InternalKey(ns_key, start_score_bytes, metadata.version).Encode(&start_key);
  InternalKey(ns_key, "", metadata.version).Encode(&prefix_key);

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(db_);
  read_options.snapshot = ss.GetSnapShot();
  read_options.fill_cache = false;

  int pos = 0;
  auto iter = db_->NewIterator(read_options, score_cf_handle_);
  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisZSet);
  batch.PutLogData(log_data.Encode());
  iter->Seek(start_key);
  // when using reverse range, we should use the `SeekForPrev` to put the iter to the right position in those cases:
  //    a. the key with the max score was the maximum key in DB and the iter would be invalid, try to seek the prev key
  //    b. there's some key after the key with max score and the iter would be valid, but the current key may be:
  //        b1. the prefix was the same with start key, don't skip it (the zset key has existed)
  //        b2. the prefix was not the same with start key, try to seek the prev key
  // Note: the DB key was composed by `NS|key|version|score|member`, so SeekForPrev(`NS|key|version|score`)
  // may skip the current score if exists, so do SeekForPrev if prefix was not the same with start key
  if (spec.reversed && (!iter->Valid() || !iter->key().starts_with(prefix_key))) {
    iter->SeekForPrev(start_key);
  }
  for (;
      iter->Valid() && iter->key().starts_with(prefix_key);
      !spec.reversed ? iter->Next() : iter->Prev()) {
    InternalKey ikey(iter->key());
    Slice score_key = ikey.GetSubKey();
    double score;
    GetDouble(&score_key, &score);
    if (spec.reversed) {
      if ((spec.minex && score == spec.min) || score < spec.min) break;
      if ((spec.maxex && score == spec.max) || score > spec.max) continue;
    } else {
      if ((spec.minex && score == spec.min) || score < spec.min) continue;
      if ((spec.maxex && score == spec.max) || score > spec.max) break;
    }
    if (spec.offset >= 0 && pos++ < spec.offset) continue;
    if (spec.removed) {
      std::string sub_key;
      InternalKey(ns_key, score_key, metadata.version).Encode(&sub_key);
      batch.Delete(sub_key);
      batch.Delete(score_cf_handle_, iter->key());
    } else {
      if (mscores) mscores->emplace_back(MemberScore{score_key.ToString(), score});
    }
    if (size) *size += 1;
    if (spec.count > 0 && mscores && mscores->size() >= static_cast<unsigned>(spec.count)) break;
  }
  delete iter;

  if (spec.removed && *size > 0) {
    metadata.size -= *size;
    std::string bytes;
    metadata.Encode(&bytes);
    batch.Put(metadata_cf_handle_, ns_key, bytes);
    return storage_->Write(rocksdb::WriteOptions(), &batch);
  }
  return rocksdb::Status::OK();
}

rocksdb::Status ZSet::RangeByLex(const Slice &user_key,
                                 ZRangeLexSpec spec,
                                 std::vector<std::string> *members,
                                 int *size) {
  if (size) *size = 0;
  if (members) members->clear();

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  ZSetMetadata metadata(true);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound() ? rocksdb::Status::OK() : s;

  std::string start_key, prefix_key;
  InternalKey(ns_key, spec.min, metadata.version).Encode(&start_key);
  InternalKey(ns_key, "", metadata.version).Encode(&prefix_key);

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(db_);
  read_options.snapshot = ss.GetSnapShot();
  read_options.fill_cache = false;

  int pos = 0;
  auto iter = db_->NewIterator(read_options);
  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisZSet);
  batch.PutLogData(log_data.Encode());
  for (iter->Seek(start_key);
       iter->Valid() && iter->key().starts_with(prefix_key);
       iter->Next()) {
    InternalKey ikey(iter->key());
    Slice member = ikey.GetSubKey();
    if (spec.minex && member == spec.min) continue;  // the min score was exclusive
    if ((spec.maxex && member == spec.max) || (!spec.max_infinite && member.ToString() > spec.max)) break;
    if (spec.offset >= 0 && pos++ < spec.offset) continue;
    if (spec.removed) {
      std::string score_bytes = iter->value().ToString();
      score_bytes.append(member.ToString());
      std::string score_key;
      InternalKey(ns_key, score_bytes, metadata.version).Encode(&score_key);
      batch.Delete(score_cf_handle_, score_key);
      batch.Delete(iter->key());
    } else {
      if (members) members->emplace_back(member.ToString());
    }
    if (size) *size += 1;
    if (spec.count > 0 && members && members->size() >= static_cast<unsigned>(spec.count)) break;
  }
  delete iter;

  if (spec.removed && *size > 0) {
    metadata.size -= *size;
    std::string bytes;
    metadata.Encode(&bytes);
    batch.Put(metadata_cf_handle_, ns_key, bytes);
    return storage_->Write(rocksdb::WriteOptions(), &batch);
  }
  return rocksdb::Status::OK();
}

rocksdb::Status ZSet::Score(const Slice &user_key, const Slice &member, double *score) {
  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);
  ZSetMetadata metadata(true);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s;

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(db_);
  read_options.snapshot = ss.GetSnapShot();

  std::string member_key, score_bytes;
  InternalKey(ns_key, member, metadata.version).Encode(&member_key);
  s = db_->Get(read_options, member_key, &score_bytes);
  if (!s.ok()) return s;
  *score = DecodeDouble(score_bytes.data());
  return rocksdb::Status::OK();
}

rocksdb::Status ZSet::Remove(const Slice &user_key, const std::vector<Slice> &members, int *ret) {
  *ret = 0;
  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  ZSetMetadata metadata(true);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound()? rocksdb::Status::OK():s;

  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisZSet);
  batch.PutLogData(log_data.Encode());
  int removed = 0;
  std::string member_key, score_key;
  for (const auto &member : members) {
    InternalKey(ns_key, member, metadata.version).Encode(&member_key);
    std::string score_bytes;
    s = db_->Get(rocksdb::ReadOptions(), member_key, &score_bytes);
    if (s.ok()) {
      score_bytes.append(member.ToString());
      InternalKey(ns_key, score_bytes, metadata.version).Encode(&score_key);
      batch.Delete(member_key);
      batch.Delete(score_cf_handle_, score_key);
      removed++;
    }
  }
  if (removed > 0) {
    *ret = removed;
    metadata.size -= removed;
    std::string bytes;
    metadata.Encode(&bytes);
    batch.Put(metadata_cf_handle_, ns_key, bytes);
  }
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}

rocksdb::Status ZSet::RemoveRangeByScore(const Slice &user_key, ZRangeSpec spec, int *ret) {
  spec.removed = true;
  return RangeByScore(user_key, spec, nullptr, ret);
}

rocksdb::Status ZSet::RemoveRangeByLex(const Slice &user_key, ZRangeLexSpec spec, int *ret) {
  spec.removed = true;
  return RangeByLex(user_key, spec, nullptr, ret);
}

rocksdb::Status ZSet::RemoveRangeByRank(const Slice &user_key, int start, int stop, int *ret) {
  uint8_t flags = ZSET_REMOVED;
  std::vector<MemberScore> mscores;
  rocksdb::Status s = Range(user_key, start, stop, flags, &mscores);
  *ret = static_cast<int>(mscores.size());
  return s;
}

rocksdb::Status ZSet::Rank(const Slice &user_key, const Slice &member, bool reversed, int *ret) {
  *ret = -1;

  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);
  ZSetMetadata metadata(true);
  rocksdb::Status s = GetMetadata(ns_key, &metadata);
  if (!s.ok()) return s.IsNotFound()? rocksdb::Status::OK():s;

  rocksdb::ReadOptions read_options;
  LatestSnapShot ss(db_);
  read_options.snapshot = ss.GetSnapShot();
  std::string score_bytes, member_key;
  InternalKey(ns_key, member, metadata.version).Encode(&member_key);
  s = db_->Get(rocksdb::ReadOptions(), member_key, &score_bytes);
  if (!s.ok()) return s.IsNotFound()? rocksdb::Status::OK():s;

  double target_score = DecodeDouble(score_bytes.data());
  std::string start_score_bytes, start_key, prefix_key;
  double start_score = !reversed ? std::numeric_limits<double>::lowest():std::numeric_limits<double>::max();
  PutDouble(&start_score_bytes, start_score);
  InternalKey(ns_key, start_score_bytes, metadata.version).Encode(&start_key);
  InternalKey(ns_key, "", metadata.version).Encode(&prefix_key);

  int rank = 0;
  read_options.fill_cache = false;
  auto iter = db_->NewIterator(read_options, score_cf_handle_);
  iter->Seek(start_key);
  // see comment in rangebyscore()
  if (reversed && (!iter->Valid() || !iter->key().starts_with(prefix_key))) {
    iter->SeekForPrev(start_key);
  }
  for (;
      iter->Valid() && iter->key().starts_with(prefix_key);
      !reversed ? iter->Next() : iter->Prev()) {
    InternalKey ikey(iter->key());
    Slice score_key = ikey.GetSubKey();
    double score;
    GetDouble(&score_key, &score);
    if (score == target_score && score_key == member) break;
    rank++;
  }
  delete iter;

  *ret = rank;
  return rocksdb::Status::OK();
}

rocksdb::Status ZSet::Overwrite(const Slice &user_key, const std::vector<MemberScore> &mscores) {
  std::string ns_key;
  AppendNamespacePrefix(user_key, &ns_key);

  LockGuard guard(storage_->GetLockManager(), ns_key);
  ZSetMetadata metadata;
  rocksdb::WriteBatch batch;
  WriteBatchLogData log_data(kRedisZSet);
  batch.PutLogData(log_data.Encode());
  for (const auto &ms : mscores) {
    std::string member_key, score_bytes, score_key;
    InternalKey(ns_key, ms.member, metadata.version).Encode(&member_key);
    PutDouble(&score_bytes, ms.score);
    batch.Put(member_key, score_bytes);
    score_bytes.append(ms.member);
    InternalKey(ns_key, score_bytes, metadata.version).Encode(&score_key);
    batch.Put(score_cf_handle_, score_key, Slice());
  }
  metadata.size = static_cast<uint32_t>(mscores.size());
  std::string bytes;
  metadata.Encode(&bytes);
  batch.Put(metadata_cf_handle_, ns_key, bytes);
  return storage_->Write(rocksdb::WriteOptions(), &batch);
}

rocksdb::Status ZSet::InterStore(const Slice &dst,
                                 const std::vector<KeyWeight> &keys_weights,
                                 AggregateMethod aggregate_method,
                                 int *size) {
  if (size) *size = 0;

  std::map<std::string, double> dst_zset;
  std::map<std::string, size_t> member_counters;
  std::vector<MemberScore> target_mscores;
  int target_size;
  ZRangeSpec spec;
  auto s = RangeByScore(keys_weights[0].key, spec, &target_mscores, &target_size);
  if (!s.ok() || target_mscores.empty()) return s;
  for (const auto &ms : target_mscores) {
    double score = ms.score * keys_weights[0].weight;
    dst_zset[ms.member] = score;
    member_counters[ms.member] = 1;
  }
  for (size_t i = 1; i < keys_weights.size(); i++) {
    auto s = RangeByScore(keys_weights[i].key, spec, &target_mscores, &target_size);
    if (!s.ok() || target_mscores.empty()) return s;
    for (const auto &ms : target_mscores) {
      if (dst_zset.find(ms.member) == dst_zset.end()) continue;
      member_counters[ms.member]++;
      double score = ms.score * keys_weights[i].weight;
      switch (aggregate_method) {
        case kAggregateSum:dst_zset[ms.member] += score;
          break;
        case kAggregateMin:
          if (dst_zset[ms.member] > score) {
            dst_zset[ms.member] = score;
          }
          break;
        case kAggregateMax:
          if (dst_zset[ms.member] < score) {
            dst_zset[ms.member] = score;
          }
          break;
      }
    }
  }
  if (!dst_zset.empty()) {
    std::vector<MemberScore> mscores;
    for (const auto &iter : dst_zset) {
      if (member_counters[iter.first] != keys_weights.size()) continue;
      mscores.emplace_back(MemberScore{iter.first, iter.second});
    }
    if (size) *size = mscores.size();
    Overwrite(dst, mscores);
  }

  return rocksdb::Status::OK();
}

rocksdb::Status ZSet::UnionStore(const Slice &dst,
                                 const std::vector<KeyWeight> &keys_weights,
                                 AggregateMethod aggregate_method,
                                 int *size) {
  if (size) *size = 0;

  std::map<std::string, double> dst_zset;
  std::vector<MemberScore> target_mscores;
  int target_size;
  ZRangeSpec spec;
  for (const auto &key_weight : keys_weights) {
    // get all member
    auto s = RangeByScore(key_weight.key, spec, &target_mscores, &target_size);
    if (!s.ok() && !s.IsNotFound()) return s;
    for (const auto &ms : target_mscores) {
      double score = ms.score * key_weight.weight;
      if (dst_zset.find(ms.member) == dst_zset.end()) {
        dst_zset[ms.member] = score;
      } else {
        switch (aggregate_method) {
          case kAggregateSum:dst_zset[ms.member] += score;
            break;
          case kAggregateMin:
            if (dst_zset[ms.member] > score) {
              dst_zset[ms.member] = score;
            }
            break;
          case kAggregateMax:
            if (dst_zset[ms.member] < score) {
              dst_zset[ms.member] = score;
            }
            break;
        }
      }
    }
  }
  if (!dst_zset.empty()) {
    std::vector<MemberScore> mscores;
    for (const auto &iter : dst_zset) {
      mscores.emplace_back(MemberScore{iter.first, iter.second});
    }
    if (size) *size = mscores.size();
    Overwrite(dst, mscores);
  }

  return rocksdb::Status::OK();
}

Status ZSet::ParseRangeSpec(const std::string &min, const std::string &max, ZRangeSpec *spec) {
  const char *sptr = nullptr;
  char *eptr = nullptr;

  if (min == "+inf" ||  max == "-inf") {
    return Status(Status::NotOK, "min > max");
  }

  if (min == "-inf") {
    spec->min = std::numeric_limits<double>::lowest();
  } else {
    sptr = min.data();
    if (!min.empty() && min[0] == '(') {
      spec->minex = true;
      sptr++;
    }
    spec->min = strtod(sptr, &eptr);
    if ((eptr && eptr[0] != '\0') || isnan(spec->min)) {
      return Status(Status::NotOK, "the min isn't double");
    }
  }

  if (max == "+inf") {
    spec->max = std::numeric_limits<double>::max();
  } else {
    sptr = max.data();
    if (!max.empty() && max[0] == '(') {
      spec->maxex = true;
      sptr++;
    }
    spec->max = strtod(sptr, &eptr);
    if ((eptr && eptr[0] != '\0') || isnan(spec->max)) {
      return Status(Status::NotOK, "the max isn't double");
    }
  }
  return Status::OK();
}

Status ZSet::ParseRangeLexSpec(const std::string &min, const std::string &max, ZRangeLexSpec *spec) {
  if (min == "+" || max == "-") {
    return Status(Status::NotOK, "min > max");
  }

  if (min == "-") {
    spec->min = "";
  } else {
    if (min[0] == '(') {
      spec->minex = true;
    } else if (min[0] == '[') {
      spec->minex = false;
    } else {
      return Status(Status::NotOK, "the min is illegal");
    }
    spec->min = min.substr(1);
  }

  if (max == "+") {
    spec->max_infinite = true;
  } else {
    if (max[0] == '(') {
      spec->maxex = true;
    } else if (max[0] == '[') {
      spec->maxex = false;
    } else {
      return Status(Status::NotOK, "the max is illegal");
    }
    spec->max = max.substr(1);
  }
  return Status::OK();
}

rocksdb::Status ZSet::Scan(const Slice &user_key,
                                const std::string &cursor,
                                uint64_t limit,
                                const std::string &member_prefix,
                                std::vector<std::string> *members) {
  return SubKeyScanner::Scan(kRedisZSet, user_key, cursor, limit, member_prefix, members);
}

}  // namespace Redis
