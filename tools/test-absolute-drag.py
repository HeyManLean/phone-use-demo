"""Host regression test for the actual firmware drag branch/parser (no board needed)."""
from pathlib import Path
import re
repo = Path(__file__).resolve().parents[1]
src = (repo / 'hardware/hid_bridge/hid_bridge.ino').read_text(encoding='utf-8')
parser = src[src.index('static int parsePoints'):src.index('String run(')]
drag = src[src.index('  if (cmd == "drag")'):src.index('  if (cmd == "scroll")')]
parser = 'const int MAXP = ' + re.search(r'const int MAXP = (\d+);', src).group(1) + ';\n' + parser
harness = r'''
#include <string>
#include <vector>
#include <cstdlib>
#include <cstring>
#include <cassert>
#include <algorithm>
using std::max;
struct String : std::string { using std::string::string; int indexOf(const char *s) const { auto p=find(s); return p==npos ? -1 : int(p); } };
template<class T> T constrain(T v,T a,T b){return std::min(std::max(v,a),b);}
const long PTR_MAX=65535; const int DRAG_STEP_MS=8,MOUSE_LEFT=1;
long ptrx=0,ptry=0; int elapsed=0;
struct Report {long x,y; bool down;};
std::vector<Report> reports;
struct Mouse { bool down=false; void press(int){down=true;reports.push_back({ptrx,ptry,down});} void release(int){down=false;reports.push_back({ptrx,ptry,down});} } mouse;
void ptrSet(long x,long y){ptrx=x;ptry=y;reports.push_back({x,y,mouse.down});}
void delay(int ms){elapsed+=ms;}
'''
harness += parser + '\nString runDrag(String arg){String cmd="drag";\n' + drag + '\nreturn "err";}\n'
harness += r'''
int main(){
 assert(runDrag("10000 10000 50000 50000 hold")=="ok");
 assert(ptrx==50000 && ptry==50000 && !mouse.down);
 int held=0; for(auto r:reports)if(r.down)held++;
 assert(held>20 && elapsed>=430);
 auto count=reports.size();
 assert(runDrag("1 2 3 4 garbage").find("err")==0 && reports.size()==count);
 assert(runDrag("1 2 3").find("err")==0 && reports.size()==count);
 assert(runDrag("1 2 3 70000").find("err")==0 && reports.size()==count);
 String many; for(int i=0;i<MAXP/2+1;i++) many+="100 200 ";
 assert(runDrag(many).find("err")==0 && reports.size()==count);
 String full; for(int i=0;i<MAXP/2;i++) full+="100 200 ";
 assert(runDrag(full)=="ok");
 String circle; for(int i=0;i<25;i++) circle+="100 200 ";
 assert(runDrag(circle)=="ok");
}
'''
(repo / '.tmp/drag-regression.cpp').write_text(harness, encoding='utf-8')
print('Generated host tests from actual firmware parser and drag branch.')
