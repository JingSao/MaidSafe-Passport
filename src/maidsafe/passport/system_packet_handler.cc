/*
* ============================================================================
*
* Copyright [2010] maidsafe.net limited
*
* Description:  Class for manipulating database of system packets
* Version:      1.0
* Created:      14/10/2010 11:43:59
* Revision:     none
* Company:      maidsafe.net limited
*
* The following source code is property of maidsafe.net limited and is not
* meant for external use.  The use of this code is governed by the license
* file LICENSE.TXT found in the root of this directory and also on
* www.maidsafe.net.
*
* You are not free to copy, amend or otherwise use this source code without
* the explicit written permission of the board of directors of maidsafe.net.
*
* ============================================================================
*/

#include "maidsafe/passport/system_packet_handler.h"

#include <cstdio>
#include <sstream>

#include "boost/archive/text_oarchive.hpp"
#include "boost/archive/text_iarchive.hpp"
#include "boost/serialization/map.hpp"

#include "maidsafe/common/utils.h"

#include "maidsafe/passport/log.h"
#include "maidsafe/passport/passport_config.h"


namespace maidsafe {

namespace passport {

bool SystemPacketHandler::AddPendingPacket(
    std::shared_ptr<pki::Packet> packet) {
  if (!packet)
    return false;
  boost::mutex::scoped_lock lock(mutex_);
  SystemPacketMap::iterator it =
      packets_.find(static_cast<PacketType>(packet->packet_type()));
  if (it == packets_.end()) {
    std::pair<SystemPacketMap::iterator, bool> result =
        packets_.insert(SystemPacketMap::value_type(
            static_cast<PacketType>(packet->packet_type()),
            PacketInfo(packet)));
#ifdef DEBUG
    if (!result.second)
      DLOG(ERROR) << "SystemPacketHandler::AddPacket: Failed for "
                  << DebugString(packet->packet_type()) << std::endl;
#endif
    return result.second;
  } else {
    (*it).second.pending = packet;
    return true;
  }
}

int SystemPacketHandler::ConfirmPacket(
    std::shared_ptr<pki::Packet> packet) {
  if (!packet)
    return kNullPointer;
  PacketType packet_type = static_cast<PacketType>(packet->packet_type());
  boost::mutex::scoped_lock lock(mutex_);
  SystemPacketMap::iterator it = packets_.find(packet_type);
  if (it == packets_.end()) {
     DLOG(ERROR) << "SystemPacketHandler::ConfirmPacket: Missing "
                 << DebugString(packet_type) << std::endl;
    return kNoPendingPacket;
  }
  if (!(*it).second.pending) {
    if ((*it).second.stored && (*it).second.stored->Equals(packet))
      return kSuccess;
    DLOG(ERROR) << "SystemPacketHandler::ConfirmPacket: Missing "
                << DebugString(packet_type) << std::endl;
    return kNoPendingPacket;
  }
  bool dependencies_confirmed(true);
  switch (packet_type) {
    case MID:
      dependencies_confirmed = IsConfirmed(packets_.find(ANMID));
      break;
    case SMID:
      dependencies_confirmed = IsConfirmed(packets_.find(ANSMID));
      break;
    case TMID:
      dependencies_confirmed = IsConfirmed(packets_.find(ANTMID)) &&
                               IsConfirmed(packets_.find(MID)) &&
                               IsConfirmed(packets_.find(ANMID));
      break;
    case STMID:
      dependencies_confirmed = IsConfirmed(packets_.find(ANTMID)) &&
                               IsConfirmed(packets_.find(SMID)) &&
                               IsConfirmed(packets_.find(ANSMID));
      break;
    case MPID:
      dependencies_confirmed = IsConfirmed(packets_.find(ANMPID));
      break;
    case PMID:
      dependencies_confirmed = IsConfirmed(packets_.find(MAID)) &&
                               IsConfirmed(packets_.find(ANMAID));
      break;
    case MAID:
      dependencies_confirmed = IsConfirmed(packets_.find(ANMAID));
      break;
    default:
      break;
  }
  if (!dependencies_confirmed) {
    DLOG(ERROR) << "SystemPacketHandler::ConfirmPacket: dependencies for "
                << DebugString(packet_type) << " not confirmed" << std::endl;
    return kMissingDependentPackets;
  } else {
    if (!(*it).second.pending->Equals(packet)) {
      DLOG(ERROR) << "SystemPacketHandler::ConfirmPacket: input "
                  << DebugString(packet_type) << " != pending "
                  << DebugString(packet_type) << std::endl;

      return kPacketsNotEqual;
    }
    (*it).second.stored = (*it).second.pending;
    (*it).second.pending.reset();
    return kSuccess;
  }
}

bool SystemPacketHandler::RevertPacket(const PacketType &packet_type) {
  boost::mutex::scoped_lock lock(mutex_);
  SystemPacketMap::iterator it = packets_.find(packet_type);
  if (it == packets_.end()) {
    DLOG(ERROR) << "SystemPacketHandler::RevertPacket: Missing "
                << DebugString(packet_type) << std::endl;
    return false;
  } else {
    (*it).second.pending.reset();
    return true;
  }
}

std::shared_ptr<pki::Packet> SystemPacketHandler::GetPacket(
    const PacketType &packet_type,
    bool confirmed) {
  std::shared_ptr<pki::Packet> packet;
  boost::mutex::scoped_lock lock(mutex_);
  SystemPacketMap::iterator it = packets_.find(packet_type);
  if (it == packets_.end()) {
    DLOG(ERROR) << "SystemPacketHandler::Packet: Missing "
                << DebugString(packet_type) << std::endl;
  } else {
    std::shared_ptr<pki::Packet> retrieved_packet;
    if (confirmed && (*it).second.stored) {
      retrieved_packet = (*it).second.stored;
    } else if (!confirmed && (*it).second.pending) {
      retrieved_packet = (*it).second.pending;
    }
    if (retrieved_packet) {
      // return a copy of the contents
      if (packet_type == TMID || packet_type == STMID) {
        packet = std::shared_ptr<TmidPacket>(new TmidPacket(
            *std::static_pointer_cast<TmidPacket>(retrieved_packet)));
      } else if (packet_type == MID || packet_type == SMID) {
        packet = std::shared_ptr<MidPacket>(new MidPacket(
            *std::static_pointer_cast<MidPacket>(retrieved_packet)));
      } else if (IsSignature(packet_type, false)) {
        packet = std::shared_ptr<pki::SignaturePacket>(new pki::SignaturePacket(
            *std::static_pointer_cast<pki::SignaturePacket>(retrieved_packet)));
      } else {
        DLOG(ERROR) << "SystemPacketHandler::Packet: "
                    << DebugString(packet_type) << " type error."  << std::endl;
      }
    } else {
      DLOG(ERROR) << "SystemPacketHandler::Packet: " << DebugString(packet_type)
                  << " not "
                  << (confirmed ? "confirmed as stored." :
                                  "pending confirmation.")
                  << std::endl;
    }
  }
  return packet;
}

std::shared_ptr<pki::Packet> SystemPacketHandler::GetPacket(
    const std::string &packet_id,
    bool confirmed) {
  std::shared_ptr<pki::Packet> packet;
  boost::mutex::scoped_lock lock(mutex_);
  auto it = packets_.begin();
  bool done(false);
  for (; it != packets_.end() && !done; ++it) {
    std::shared_ptr<pki::Packet> retrieved_packet;
    if ((*it).second.stored &&
        (*it).second.stored->name() == packet_id &&
        confirmed) {
      retrieved_packet = (*it).second.stored;
    } else if ((*it).second.pending &&
               (*it).second.pending->name() == packet_id &&
               !confirmed) {
      retrieved_packet = (*it).second.pending;
    }
    if (retrieved_packet) {
      // return a copy of the contents
      done = true;
      if (retrieved_packet->packet_type() == TMID ||
          retrieved_packet->packet_type() == STMID) {
        packet = std::shared_ptr<TmidPacket>(new TmidPacket(
            *std::static_pointer_cast<TmidPacket>(retrieved_packet)));
      } else if (retrieved_packet->packet_type() == MID ||
                 retrieved_packet->packet_type() == SMID) {
        packet = std::shared_ptr<MidPacket>(new MidPacket(
            *std::static_pointer_cast<MidPacket>(retrieved_packet)));
      } else if (IsSignature(retrieved_packet->packet_type(), false)) {
        packet = std::shared_ptr<pki::SignaturePacket>(new pki::SignaturePacket(
            *std::static_pointer_cast<pki::SignaturePacket>(retrieved_packet)));
      } else {
        DLOG(ERROR) << "SystemPacketHandler::Packet: "
                    << DebugString(retrieved_packet->packet_type())
                    << " type error."  << std::endl;
      }
    }
  }
  if (!done) {
    DLOG(ERROR) << "SystemPacketHandler::Packet: not "
                << (confirmed ? "confirmed as stored." :
                                "pending confirmation.")
                << std::endl;
  }
  return packet;
}

bool SystemPacketHandler::Confirmed(const PacketType &packet_type) {
  boost::mutex::scoped_lock lock(mutex_);
  return IsConfirmed(packets_.find(packet_type));
}

bool SystemPacketHandler::IsConfirmed(SystemPacketMap::iterator it) {
  return (it != packets_.end() && !(*it).second.pending && (*it).second.stored);
}

std::string SystemPacketHandler::SerialiseKeyring(
    const std::string & /*public_name*/) {
  std::ostringstream key_chain(std::ios_base::binary);
  boost::archive::text_oarchive output_archive(key_chain);
  boost::mutex::scoped_lock lock(mutex_);
  SystemPacketMap::iterator it = packets_.begin();
  SystemPacketMap spm;
  while (it != packets_.end()) {
    if (IsSignature((*it).first, false) && (*it).second.stored) {
//      output_archive << (*it);
      spm.insert(*it);
    }
    ++it;
  }
  // Serialise map
  output_archive << spm;
  return key_chain.str();
}

int SystemPacketHandler::ParseKeyring(const std::string &serialised_keyring,
                                      std::string * /*public_name*/) {
  std::istringstream key_chain(serialised_keyring, std::ios_base::binary);
  boost::archive::text_iarchive input_archive(key_chain);

  SystemPacketMap spm;
  input_archive >> spm;
  if (spm.empty()) {
    DLOG(ERROR) << "SystemPacketHandler::ParseKeyring failed." << std::endl;
    return kBadSerialisedKeyring;
  }

  boost::mutex::scoped_lock lock(mutex_);
  for (auto it(spm.begin()); it != spm.end(); ++it) {
    auto result = packets_.insert(*it);
    if (!result.second) {
      DLOG(ERROR) << "SystemPacketHandler::ParseKeyring: Failed for "
                  << DebugString((*it).second.stored->packet_type());
      return kKeyringNotEmpty;
    }
  }

  return kSuccess;
}

void SystemPacketHandler::ClearKeyring() {
  boost::mutex::scoped_lock lock(mutex_);
  SystemPacketMap::iterator it = packets_.begin();
  while (it != packets_.end()) {
    if (IsSignature((*it).first, false)) {
      packets_.erase(it++);
    } else {
      ++it;
    }
  }
}

int SystemPacketHandler::DeletePacket(const PacketType &packet_type) {
  boost::mutex::scoped_lock lock(mutex_);
  size_t deleted_count = packets_.erase(packet_type);
  if (deleted_count == 0U) {
    DLOG(ERROR) << "SystemPacketHandler::DeletePacket: Missing "
                << DebugString(packet_type) << std::endl;
    return kNoPacket;
  } else {
    return kSuccess;
  }
}

void SystemPacketHandler::Clear() {
  boost::mutex::scoped_lock lock(mutex_);
  packets_.clear();
}

}  // namespace passport

}  // namespace maidsafe
