#pragma once

#include <chrono>
#include "elf/interface/game_interface.h"
#include "elf/base/ctrl.h"
#include "elf/distributed/addrs.h"
#include "elf/distributed/options.h"
#include "elf/distributed/shared_rw_buffer3.h"

#include "elf/base/dispatcher.h"
#include "record.h"
#include "options.h"

#include "game_interface.h"

namespace elf { 

namespace cs {

using ThreadedWriter = msg::Client;
using ThreadedDispatcher = ThreadedDispatcherT<MsgRequest, MsgReply>;

struct GuardedRecords {
 public:
  GuardedRecords(const std::string& identity) : records_(identity) {}

  void feed(Record&& r) {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.addRecord(std::move(r));
  }

  void updateState(const ThreadState& ts) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto now = elf_utils::sec_since_epoch_from_now();
    records_.updateState(ts);

    last_states_.push_back(std::make_pair(now, ts));
    if (last_states_.size() > 100) {
      last_states_.pop_front();
    }

    if (now - last_state_vis_time_ > 60) {
      std::unordered_map<int, ThreadState> states;
      std::unordered_map<int, uint64_t> timestamps;
      for (const auto& s : last_states_) {
        timestamps[s.second.thread_id] = s.first;
        states[s.second.thread_id] = s.second;
      }

      std::cout << "GuardedRecords::updateState[" << elf_utils::now() << "] "
                << visStates(states, &timestamps) << std::endl;

      last_state_vis_time_ = now;
    }
  }

  size_t size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return records_.records.size();
  }

  std::string dumpAndClear() {
    // send data.
    std::lock_guard<std::mutex> lock(mutex_);
    std::cout << "GuardedRecords::DumpAndClear[" << elf_utils::now()
              << "], #records: " << records_.records.size();

    std::cout << ", " << visStates(records_.states) << std::endl;
    std::string s = records_.dumpJsonString();
    records_.clear();
    return s;
  }

 private:
  std::mutex mutex_;
  Records records_;
  std::deque<std::pair<uint64_t, ThreadState>> last_states_;
  uint64_t last_state_vis_time_ = 0;

  static std::string visStates(
      const std::unordered_map<int, ThreadState>& states,
      const std::unordered_map<int, uint64_t>* timestamps = nullptr) {
    std::stringstream ss;
    ss << "#states: " << states.size();
    ss << "[";

    auto now = elf_utils::sec_since_epoch_from_now();
    std::vector<int> ordered;
    for (const auto& p : states) {
      ordered.push_back(p.first);
    }
    std::sort(ordered.begin(), ordered.end());

    for (const auto& th_id : ordered) {
      auto it = states.find(th_id);
      assert(it != states.end());

      ss << th_id << ":" << it->second.seq << ":" << it->second.move_idx;

      if (timestamps != nullptr) {
        auto it = timestamps->find(th_id);
        if (it != timestamps->end()) {
          uint64_t td = now - it->second;
          ss << ":" << td;
        }
        ss << ",";
      }
    }
    ss << "]  ";

    ss << elf_utils::get_gap_list(ordered);
    return ss.str();
  }
};


class WriterCallback {
 public:
  WriterCallback(ThreadedWriter* writer, Ctrl& ctrl)
      : ctrl_(ctrl), records_(writer->identity()) {
    writer->setCallbacks(
        std::bind(&WriterCallback::OnSend, this),
        std::bind(&WriterCallback::OnRecv, this, std::placeholders::_1));
    writer->start();
  }

  int64_t OnRecv(const std::string& smsg) {
    // Send data.
    std::cout << "WriterCB: RecvMsg: " << smsg << std::endl;
    ctrl_.sendMail("dispatcher", 
        MsgRequest::createFromJson(json::parse(smsg)));
    return -1;
  }

  std::string OnSend() {
    size_t sz = records_.size();
    std::string msg = records_.dumpAndClear();
    std::cout << "WriterCB: SendMsg: " << sz << std::endl;

    // If there is no record, wait a few seconds then send (to reduce server's load)
    if (sz == 0) {
      // TODO: OpenGo this is 60 seconds
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return msg;
  }

  void addRecord(const json &state) {
    Record record;
    record.request.state = state;
    // Not used. 
    // record.result.reply = r;
    records_.feed(std::move(record));
  }

  void updateState(const ThreadState &ts) {
    records_.updateState(ts);
  }

 private:
  Ctrl& ctrl_;
  GuardedRecords records_;
};

class Client {
 public:
  Client(const Options &options) : options_(options) {}

  void setFactory(ClientFactory factory) {
    factory_ = factory;
  }

  void setGameContext(elf::GCInterface* ctx) {
    uint64_t num_games = ctx->options().num_game_thread;

    if (ctx->getClient() != nullptr) {
      dispatcher_.reset(new ThreadedDispatcher(ctrl_, num_games));
    }

    auto netOptions =
      elf::msg::getNetOptions(options_.base, options_.net);
    // if no message, sleep every 10s
    netOptions.usec_sleep_when_no_msg = 10000000;
    // Resend after 900s
    netOptions.usec_resend_when_no_msg = 900000000;
    writer_.reset(new ThreadedWriter(netOptions));
    writer_callback_.reset(new WriterCallback(writer_.get(), ctrl_));

    using std::placeholders::_1;
    using std::placeholders::_2;

    for (size_t i = 0; i < num_games; ++i) {
      auto* g = ctx->getGame(i);
      if (g != nullptr) {
        games_.emplace_back(new ClientGame(i, this));
        g->setCallbacks(
            std::bind(&ClientGame::OnAct, games_[i].get(), _1),
            std::bind(&ClientGame::OnEnd, games_[i].get(), _1),
            [&, i](elf::game::Base*) { dispatcher_->RegGame(i); });
      }
    }

    if (ctx->getClient() != nullptr) {
      dispatcher_->Start(factory_.onReply, factory_.onFirstSend);
    }
  }

  std::unordered_map<std::string, int> getParams() const {
    return factory_.getParams();
  }

  ~Client() {
    dispatcher_.reset(nullptr);
    writer_.reset(nullptr);
    writer_callback_.reset(nullptr);
  }

 private:
  struct ClientGame {
    int game_idx_;
    Client *c_ = nullptr;
    std::unique_ptr<ClientInterface> game_;
    int counter_ = 0;

    ClientGame(int game_idx, Client *client) 
      : game_idx_(game_idx), c_(client) {
      game_ = std::move(c_->factory_.createClientInterface(game_idx));
    }

    bool OnReceive(const MsgRequest& request, MsgReply* reply) {
      return game_->onReceive(request.state, reply);
    }

    void OnEnd(elf::game::Base* base) {
      game_->onEnd(base);
    }

    void OnAct(elf::game::Base* base) {
      // elf::GameClient* client = base->ctx().client;
      if (counter_ % 5 == 0) {
        using std::placeholders::_1;
        using std::placeholders::_2;
        auto f = std::bind(&ClientGame::OnReceive, this, _1, _2);
        bool block_if_no_message = false;

        do {
          c_->dispatcher_->checkMessage(block_if_no_message, f);
        } while (false);

        c_->writer_callback_->updateState(game_->getThreadState());
      }
      counter_ ++;

      json j;
      if (game_->step(base, &j) == StepStatus::NEW_RECORD) {
        c_->writer_callback_->addRecord(j);
      }
    }
  };

  Ctrl ctrl_;

  const Options options_;
  ClientFactory factory_;

  std::vector<std::unique_ptr<ClientGame>> games_;
  std::unique_ptr<ThreadedDispatcher> dispatcher_;

  /// ZMQClient
  std::unique_ptr<ThreadedWriter> writer_;
  std::unique_ptr<WriterCallback> writer_callback_;
};

}  // namespace cs

}  // namespace elf
