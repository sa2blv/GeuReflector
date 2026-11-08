/**
@file   AnnounceManager.h
@brief  Queues and plays audio announcements to talk groups and single nodes
@date   2026-11-08

The AnnounceManager owns all announcement playback. It reads the [ANNOUNCE]
section (pointed to by GLOBAL/ANNOUNCE), keeps a queue per talk group and
per node, waits until the TG is free, rate limits node feedback and parses
the "ANNOUNCE ..." PTY commands.

It does not know about clients, trunks, satellites or the twin. It asks the
Reflector questions through std::function hooks (is the TG busy? which TG
is this node on?) and hands every talker-start, frame and talker-stop to
the Reflector through sigc signals together with the scope, so all routing
stays in the Reflector, which is the only one that knows the topology.

Scopes for talk group announcements:
  LOCAL  own clients (and downstream satellites) only
  TRUNK  trunk links only (GEU TCP trunks, BLV UDP trunks, satellite
         parent and twin), not the own clients
  BOTH   both of the above

\verbatim
This file is part of GeuReflector / SvxLink.
Licensed under the GNU General Public License v2 or later.
\endverbatim
*/

#ifndef ANNOUNCE_MANAGER_INCLUDED
#define ANNOUNCE_MANAGER_INCLUDED

#include <sigc++/sigc++.h>

#include <cstdint>
#include <ctime>
#include <deque>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <AsyncTimer.h>

namespace Async
{
  class Config;
};

class AnnouncePlayer;


enum class AnnounceScope { LOCAL, TRUNK, BOTH };

const char* announceScopeStr(AnnounceScope s);
bool parseAnnounceScope(const std::string& s, AnnounceScope& out);


class AnnounceManager : public sigc::trackable
{
  public:
    /** Built-in feedback clips that the Reflector can trigger */
    enum class Feedback { QSY_BLOCKED, QSY_DISABLED, NODE_BLOCKED };

    /**
     * @param cfg      Reflector configuration
     * @param section  Name of the announce section (value of GLOBAL/ANNOUNCE)
     * @param codec    Codec used towards clients (e.g. "OPUS")
     */
    AnnounceManager(const Async::Config& cfg, const std::string& section,
                    const std::string& codec);
    ~AnnounceManager(void);

    /* ---------------- Hooks the Reflector must set ---------------- */

    /** Any audio flowing on the TG right now (local, trunk, BLV, mixer)? */
    std::function<bool(uint32_t tg)> tgBusy;
    /** Is the TG configured as full duplex (mixer)? */
    std::function<bool(uint32_t tg)> tgFullDuplex;
    /** TG the node is on, or -1 if the node is gone */
    std::function<int64_t(uint32_t client_id)> nodeTg;
    /** Resolve a callsign to a client id; false if not connected */
    std::function<bool(const std::string& cs, uint32_t& client_id)> findNode;

    /* ---------------- Signals routed by the Reflector ---------------- */

    sigc::signal<void(uint32_t tg, const std::string& cs, AnnounceScope)>
        tgStart;
    sigc::signal<void(uint32_t tg, const std::vector<uint8_t>&, AnnounceScope)>
        tgFrame;
    sigc::signal<void(uint32_t tg, const std::string& cs, AnnounceScope)>
        tgStop;

    sigc::signal<void(uint32_t cid, uint32_t tg, const std::string& cs)>
        nodeStart;
    sigc::signal<void(uint32_t cid, const std::vector<uint8_t>&)> nodeFrame;
    sigc::signal<void(uint32_t cid, uint32_t tg, const std::string& cs)>
        nodeStop;

    /* ---------------- API ---------------- */

    /** Queue a clip on a talk group. scope==nullptr: use the configured
     *  default for that TG. */
    bool playTg(uint32_t tg, const std::string& file,
                const AnnounceScope* scope, std::string& err);

    /** Queue a clip for a single node */
    bool playNode(uint32_t client_id, const std::string& file,
                  std::string& err);

    /** Trigger a built-in feedback clip (rate limited, may be a no-op) */
    void feedback(uint32_t client_id, uint32_t tg, Feedback kind);

    /** Stop the current announcement and clear the queue on a TG */
    void stopTg(uint32_t tg);

    /** Stop and forget everything queued for a node */
    void stopNode(uint32_t client_id);

    /** Call for every audio packet received from a node */
    void nodeAudioSeen(uint32_t client_id);

    /** Call when a node disconnects (no signals are emitted) */
    void nodeGone(uint32_t client_id);

    /** A half-duplex TG announcement owns this TG: no local talker may
     *  grab it while this is true. */
    bool ownsTg(uint32_t tg) const;

    /** Is a TG announcement on this TG currently playing (any duplex)? */
    bool isPlayingTg(uint32_t tg) const;

    /** Scope currently playing on tg (only valid if isPlayingTg) */
    AnnounceScope playingScope(uint32_t tg) const;

    /** Callsign used as talker for announcements on tg */
    std::string callsignFor(uint32_t tg) const;

    /** Default scope for tg (TG#x/ANNOUNCE_TO_MESH, then section TO_MESH,
     *  then PTY "ANNOUNCE SCOPE" override) */
    AnnounceScope defaultScope(uint32_t tg) const;

    /**
     * @brief  Handle "ANNOUNCE <args...>" from the command PTY
     * @param  args  Stream positioned after the word ANNOUNCE
     * @param  out   Informational output (written to the PTY before OK)
     * @param  err   Error text; non-empty means the command failed
     */
    void handlePtyCommand(std::istream& args, std::ostream& out,
                          std::ostream& err);

    /** Text for "ANNOUNCE" usage */
    static const char* ptyUsage(void);

  private:
    struct Item
    {
      std::string   file;
      AnnounceScope scope = AnnounceScope::LOCAL;
      time_t        queued = 0;
    };

    struct TgState
    {
      std::deque<Item>                  queue;
      std::unique_ptr<AnnouncePlayer>   player;
      AnnounceScope                     scope = AnnounceScope::LOCAL;
      std::string                       callsign;
      bool                              hdx = true;  // decided at start
    };

    struct NodeState
    {
      std::deque<Item>                  queue;
      std::unique_ptr<AnnouncePlayer>   player;
      uint32_t                          tg = 0;
      std::string                       callsign;
      struct timespec                   last_audio = {0, 0};
      time_t                            wait_since = 0;
    };

    const Async::Config&                  m_cfg;
    std::string                           m_section;
    std::string                           m_codec;
    std::string                           m_dir;
    std::string                           m_callsign = "INFO";
    bool                                  m_to_mesh = false;
    unsigned                              m_frame_ms = 20;
    unsigned                              m_feedback_interval = 30;
    unsigned                              m_node_idle_ms = 500;
    unsigned                              m_node_max_wait = 5;
    unsigned                              m_tg_max_wait = 60;
    unsigned                              m_max_queue = 5;
    unsigned                              m_max_length = 120;
    float                                 m_gain_db = 0.0f;

    std::map<uint32_t, TgState>           m_tgs;
    std::map<uint32_t, NodeState>         m_nodes;
    std::map<uint32_t, AnnounceScope>     m_scope_override;
    std::map<std::pair<uint32_t, int>, time_t> m_last_feedback;
    std::vector<std::unique_ptr<AnnouncePlayer>> m_graveyard;
    Async::Timer                          m_poll_timer;

    AnnounceManager(const AnnounceManager&);
    AnnounceManager& operator=(const AnnounceManager&);

    std::string tgValue(uint32_t tg, const std::string& tag,
                        const std::string& def) const;
    bool resolvePath(const std::string& file, std::string& path,
                     std::string& err) const;
    std::unique_ptr<AnnouncePlayer> makePlayer(const std::string& file,
                                               std::string& err) const;
    void onPoll(Async::Timer*);
    void startTg(uint32_t tg, TgState& st);
    void finishTg(uint32_t tg, bool aborted);
    void startNode(uint32_t cid, NodeState& st);
    void finishNode(uint32_t cid);
    void bury(std::unique_ptr<AnnouncePlayer>& p);
    void updatePollTimer(void);
};

#endif /* ANNOUNCE_MANAGER_INCLUDED */
