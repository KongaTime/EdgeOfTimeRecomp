// pkzprep: turns what the build pulls out of the game into files pkztool reads.
//
//   pkzprep achievements <achievements.toml> <prefix> <extra.txt> <out.txt>
//     rexglue's achievement dump -> <prefix><id>_NAME, _DESC, _LOCKED and _SCORE, then extra.txt's rows
//   pkzprep achicons <icons dir> <plate.png> <out.png>
//     <id>.png icons on an 8x6 sheet of 64 px cells, the plate in the last cell
//   pkzprep strings <english.txt> <translation.txt> <out.txt>
//     every English row in its order, with the translation's text where it has one

#include "pkzprep.h"

using namespace pkzprep;

int main(int argc, char **argv) {
  const std::string command = argc > 1 ? argv[1] : "";
  if (command == "achievements" && argc == 6)
    return Achievements(argv[2], argv[3], argv[4], argv[5]);
  if (command == "achicons" && argc == 5)
    return AchievementIcons(argv[2], argv[3], argv[4]);
  if (command == "strings" && argc == 5)
    return Strings(argv[2], argv[3], argv[4]);
  std::fprintf(stderr, "usage: pkzprep achievements <achievements.toml> <prefix> <extra.txt> <out.txt>\n"
                       "       pkzprep achicons <icons dir> <plate.png> <out.png>\n"
                       "       pkzprep strings <english.txt> <translation.txt> <out.txt>\n");
  return 2;
}
