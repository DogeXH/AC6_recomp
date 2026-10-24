#include <charconv>
#include <cstdio>
#include <string_view>

extern "C" int rex_gpu_replay_trace(const char* trace_path, const char* output_path,
                                    int frame_index, int command_index);

namespace {

bool ParseInt(std::string_view text, int& value_out) {
  const char* begin = text.data();
  const char* end = begin + text.size();
  auto result = std::from_chars(begin, end, value_out);
  return result.ec == std::errc() && result.ptr == end;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3 || argc > 5) {
    std::fprintf(stderr,
                 "Usage: %s TRACE.xtr OUTPUT.png [FRAME_INDEX=0] "
                 "[COMMAND_INDEX=-1]\n",
                 argv[0]);
    return 2;
  }

  int frame_index = 0;
  int command_index = -1;
  if ((argc >= 4 && !ParseInt(argv[3], frame_index)) ||
      (argc >= 5 && !ParseInt(argv[4], command_index))) {
    std::fprintf(stderr, "FRAME_INDEX and COMMAND_INDEX must be integers\n");
    return 2;
  }

  return rex_gpu_replay_trace(argv[1], argv[2], frame_index, command_index);
}
