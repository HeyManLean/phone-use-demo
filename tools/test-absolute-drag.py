"""Host regression test for the actual firmware drag branch/parser/validation (no board needed)."""
from pathlib import Path
import re
repo = Path(__file__).resolve().parents[1]
src = (repo / 'hardware/hid_bridge/hid_bridge.ino').read_text(encoding='utf-8')


def body(start, end):
    """The real firmware source between two markers."""
    i = src.index(start)
    return src[i:src.index(end, i)]


maxp = re.search(r'const int MAXP = (\d+);', src).group(1)
parser = f'const int MAXP = {maxp};\n' + body('static int parsePoints', 'static bool hasHold')
has_hold = body('static bool hasHold', 'String run(')
# the validation runs before any USB work; keep that property visible to the test
validation = body('  // Validate before touching USB', '\n  if (cmd == "ping") return "ok";')
drag = body('  if (cmd == "drag") {', '  if (cmd == "scroll")')
# the branch body: from its opening brace to the matching close, taken from the whole file so the
# marker text ("if (cmd == "drag") {") is not what gets balanced
open_at = src.index('if (cmd == "drag") {                                           // press') + len('if (cmd == "drag") ')
depth = 0
for i in range(open_at, len(src)):
    depth += src[i] == '{'
    depth -= src[i] == '}'
    if depth == 0:
        drag = src[open_at:i + 1]
        break
else:
    raise SystemExit('could not find the end of the drag branch')

harness = r'''
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <algorithm>
using std::max;
struct String : std::string {
  using std::string::string;
  // Arduino's String has both overloads; c_str() lets find() see a NUL-terminated string.
  int indexOf(char c) const { auto p = std::string::find(c); return p == npos ? -1 : int(p); }
  int indexOf(const char *s) const { auto p = std::string::find(s); return p == npos ? -1 : int(p); }
  String substring(size_t from) const { String r; static_cast<std::string &>(r) = std::string::substr(from); return r; }
};
template <class T> T constrain(T v, T a, T b) { return std::min(std::max(v, a), b); }
const long PTR_MAX = 65535; const int DRAG_STEP_MS = 8, MOUSE_LEFT = 1;
long ptrx = 0, ptry = 0; int elapsed = 0;
struct Report { long x, y; bool down; };
std::vector<Report> reports;
struct Mouse {
  bool down = false;
  void press(int) { down = true; reports.push_back({ptrx, ptry, down}); }
  void release(int) { down = false; reports.push_back({ptrx, ptry, down}); }
} mouse;
void ptrSet(long x, long y) { ptrx = x; ptry = y; reports.push_back({x, y, mouse.down}); }
void delay(int ms) { elapsed += ms; }
bool usbReady() { return true; }
void ptrMove(long dx, long dy) { ptrSet(ptrx + dx, ptry + dy); }
'''
harness += parser + has_hold
# one function that mirrors the real `run`: the validation block first, then the drag branch
harness += ('\nString runDrag(String arg) {\n'
            '  int sp = arg.indexOf(\' \');\n'
            '  String cmd = "drag", af = arg.substring(sp + 1);\n'
            '  long a = 0, b = 0;\n'
            '  sscanf(af.c_str(), "%ld %ld", &a, &b);\n'
            + validation +
            '  if (cmd == "drag") ' + drag +
            '\n  return "err";\n}\n')
harness += r'''
int main() {
  // a valid two-point stroke: held reports along the way, with a dwell before and after the press
  assert(runDrag("10000 10000 50000 50000 hold") == "ok");
  assert(ptrx == 50000 && ptry == 50000 && !mouse.down);
  int held = 0;
  for (auto r : reports) if (r.down) held++;
  assert(held > 20 && elapsed >= 430);

  // validation happens before any HID work: these must not move the pointer or press a button
  auto count = reports.size();
  long x = ptrx, y = ptry;
  assert(runDrag("1 2 3 4 garbage").find("err") == 0);  assert(reports.size() == count && ptrx == x && ptry == y);
  assert(runDrag("1 2 3").find("err") == 0);            assert(reports.size() == count && ptrx == x && ptry == y);
  assert(runDrag("1 2 3 70000").find("err") == 0);      assert(reports.size() == count && ptrx == x && ptry == y);

  String many;  for (int i = 0; i < MAXP / 2 + 1; i++) many += "100 200 ";
  assert(runDrag(many).find("err") == 0 && reports.size() == count);
  String full;  for (int i = 0; i < MAXP / 2; i++) full += "100 200 ";
  assert(runDrag(full) == "ok");
  String circle; for (int i = 0; i < 25; i++) circle += "100 200 ";
  assert(runDrag(circle) == "ok");
}
'''
(repo / '.tmp/drag-regression.cpp').write_text(harness, encoding='utf-8')
print('Generated host tests from the actual firmware parser, validation and drag branch.')
