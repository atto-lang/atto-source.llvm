#pragma once
// Host-side, allocation-free, bounded RFC6455 client-frame parser for the
// first-party LiveView TEXT profile. Not an integrated Oreslang guest socket.
// An owning actor/host adapter must supply monotonic deadlines, admission,
// close / cancellation, and a CONTROL-safe handoff for callbacks.
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace oreslang::liveview {
constexpr std::size_t kMaxMessage = 65536;
constexpr std::size_t kMaxFrame = kMaxMessage + 14;
constexpr unsigned kMaxFragments = 64;
constexpr unsigned kMaxFramesPerFeed = 256;

enum class Kind : std::uint8_t { text, ping, pong, close };
struct Event { Kind kind; const std::uint8_t *data; std::size_t size; };
// RFC6455 close code describing rejection. 0 means no error.
enum class Error : int { none=0, protocol=1002, unsupported=1003, utf8=1007, policy=1008, too_large=1009 };

inline bool valid_utf8(const std::uint8_t *p, std::size_t n) noexcept {
  for (std::size_t i=0;i<n;) {
    const unsigned c=p[i++];
    if (c<=0x7f) continue;
    if (c>=0xc2 && c<=0xdf) {
      if (i>=n || (p[i++]&0xc0)!=0x80) return false;
      continue;
    }
    if (c>=0xe0 && c<=0xef) {
      if (i+1>=n) return false;
      const unsigned b=p[i++];
      if ((b&0xc0)!=0x80 || (c==0xe0 && b<0xa0) ||
          (c==0xed && b>0x9f) || (p[i++]&0xc0)!=0x80) return false;
      continue;
    }
    if (c>=0xf0 && c<=0xf4) {
      if (i+2>=n) return false;
      const unsigned b=p[i++];
      if ((b&0xc0)!=0x80 || (c==0xf0 && b<0x90) ||
          (c==0xf4 && b>0x8f) || (p[i++]&0xc0)!=0x80 ||
          (p[i++]&0xc0)!=0x80) return false;
      continue;
    }
    return false;
  }
  return true;
}
inline bool valid_close_code(unsigned c) noexcept {
  return c>=1000 && c<5000 && c!=1004 && c!=1005 && c!=1006 &&
         c!=1015 && (c<1016 || c>=3000);
}

class Decoder {
 public:
  Decoder()=default;
  Decoder(const Decoder&)=delete;
  Decoder& operator=(const Decoder&)=delete;
  [[nodiscard]] Error error() const noexcept { return error_; }
  [[nodiscard]] bool closed() const noexcept { return closed_; }
  [[nodiscard]] bool fragmented() const noexcept { return fragmented_; }
  void reset() noexcept {
    used_=0; fragment_bytes_=0; fragment_count_=0; fragmented_=false;
    closed_=false; error_=Error::none;
  }

  // Callback's Event bytes are an ephemeral borrowed view; never retain them.
  // The host must not invoke guest callbacks on an I/O reactor thread.
  template<class Sink>
  Error feed(const std::uint8_t *input, std::size_t size, Sink&& sink) noexcept {
    if(error_!=Error::none) return error_;
    if(closed_) return fail(Error::protocol);
    if(size!=0 && !input) return fail(Error::protocol);
    unsigned frames=0;
    for(std::size_t at=0;at<size;++at) {
      if(used_>=frame_.size()) return fail(Error::too_large);
      frame_[used_++]=input[at];
      const Error e=probe();
      if(e!=Error::none) return fail(e);
      std::size_t total=0;
      if(!frame_total(total) || used_!=total) continue;
      if(++frames>kMaxFramesPerFeed) return fail(Error::too_large);
      const Error parsed=complete(sink);
      if(parsed!=Error::none) return fail(parsed);
      used_=0;
      if(closed_) {
        // Ignore bytes after a complete CLOSE rather than dispatching them.
        return Error::none;
      }
    }
    return Error::none;
  }
 private:
  // Stable storage is actor-owned, with no allocation on frame admission.
  std::array<std::uint8_t,kMaxFrame> frame_{};
  std::array<std::uint8_t,kMaxMessage> fragments_{};
  std::size_t used_=0,fragment_bytes_=0;
  unsigned fragment_count_=0;
  bool fragmented_=false,closed_=false;
  Error error_=Error::none;
  Error fail(Error e) noexcept { error_=e; return e; }

  bool frame_total(std::size_t& total) const noexcept {
    if(used_<2) return false;
    const unsigned marker=frame_[1]&0x7f;
    const std::size_t ext=marker==126?2:marker==127?8:0;
    if(used_<2+ext+4) return false;
    std::uint64_t length=marker;
    if(ext==2) length=(std::uint64_t(frame_[2])<<8)|frame_[3];
    if(ext==8) {
      length=0;
      for(std::size_t i=2;i<10;++i)length=(length<<8)|frame_[i];
    }
    if(length>kMaxMessage) return false;
    total=2+ext+4+static_cast<std::size_t>(length);
    return true;
  }
  Error probe() const noexcept {
    if(used_<2) return Error::none;
    const unsigned first=frame_[0], second=frame_[1],op=first&15,marker=second&127;
    const bool control=op>=8;
    if((first&0x70)!=0 || (second&0x80)==0) return Error::protocol;
    if(op==2) return Error::unsupported;  // first-party LiveView text-only
    if(op!=0 && op!=1 && op!=8 && op!=9 && op!=10) return Error::protocol;
    if(control && ((first&0x80)==0 || marker>125)) return Error::protocol;
    const std::size_t ext=marker==126?2:marker==127?8:0;
    if(used_<2+ext) return Error::none;
    std::uint64_t length=marker;
    if(ext==2) {
      length=(std::uint64_t(frame_[2])<<8)|frame_[3];
      if(length<126) return Error::protocol;
    }
    if(ext==8) {
      if(frame_[2]&0x80) return Error::protocol;
      length=0;
      for(std::size_t i=2;i<10;++i)length=(length<<8)|frame_[i];
      if(length<65536) return Error::protocol;
    }
    if(length>kMaxMessage) return Error::too_large;
    if(control && length>125) return Error::protocol;
    return Error::none;
  }
  template<class Sink> Error complete(Sink& sink) noexcept {
    const unsigned op=frame_[0]&15;const bool fin=(frame_[0]&0x80)!=0;
    const unsigned marker=frame_[1]&127;
    const std::size_t ext=marker==126?2:marker==127?8:0;
    const std::size_t start=2+ext+4;
    const std::size_t len=used_-start;
    for(std::size_t i=0;i<len;++i)frame_[start+i]^=frame_[start-4+(i&3)];
    const std::uint8_t *bytes=frame_.data()+start;
    if(op>=8) {
      if(op==8) {
        if(len==1) return Error::protocol;
        if(len>=2) {
          const unsigned code=(unsigned(bytes[0])<<8)|bytes[1];
          if(!valid_close_code(code)) return Error::protocol;
          if(!valid_utf8(bytes+2,len-2)) return Error::utf8;
        }
        sink(Event{Kind::close,bytes,len});closed_=true;
      } else sink(Event{op==9?Kind::ping:Kind::pong,bytes,len});
      return Error::none;
    }
    if(op==0 && !fragmented_) return Error::protocol;
    if(op==1 && fragmented_) return Error::protocol;
    if(op==1) { fragmented_=true;fragment_bytes_=0;fragment_count_=0; }
    if(++fragment_count_>kMaxFragments || len>kMaxMessage-fragment_bytes_)
      return Error::too_large;
    for(std::size_t i=0;i<len;++i)fragments_[fragment_bytes_+i]=bytes[i];
    fragment_bytes_+=len;
    if(fin) {
      if(!valid_utf8(fragments_.data(),fragment_bytes_)) return Error::utf8;
      sink(Event{Kind::text,fragments_.data(),fragment_bytes_});
      fragmented_=false;fragment_bytes_=0;fragment_count_=0;
    }
    return Error::none;
  }
};
} // namespace oreslang::liveview
