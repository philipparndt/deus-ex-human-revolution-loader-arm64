// C++ runtime test: exceptions, std::string (COW ABI), iostreams, containers, RTTI.
#include <cstdio>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <typeinfo>
#include <vector>

struct Base { virtual ~Base() {} };
struct Derived : Base { int v; Derived() : v(42) {} };

static std::string global_str("static init works");

static int fails = 0;
#define CHECK(c) do { if (c) printf("PASS %s\n", #c); else { printf("FAIL %s\n", #c); fails++; } } while (0)

static void thrower(int x) {
    if (x > 2) throw std::runtime_error("boom");
}

int main() {
    CHECK(global_str == "static init works");
    std::string a = "Deus", b = a;           // COW share
    b += " Ex";
    CHECK(a == "Deus" && b == "Deus Ex");
    std::ostringstream os;
    os << "Adam " << 2027 << ' ' << 3.5;
    CHECK(os.str() == "Adam 2027 3.5");
    std::istringstream is("12 34");
    int x = 0, y = 0;
    is >> x >> y;
    CHECK(x == 12 && y == 34);
    std::map<std::string, int> m;
    m["sarif"] = 1; m["belltower"] = 2;
    CHECK(m.size() == 2 && m["belltower"] == 2);
    std::vector<int> v;
    for (int i = 0; i < 1000; i++) v.push_back(i);
    CHECK(v[999] == 999);
    Base *p = new Derived;
    CHECK(dynamic_cast<Derived *>(p) && dynamic_cast<Derived *>(p)->v == 42);
    CHECK(typeid(*p) == typeid(Derived));
    delete p;
    bool caught = false;
    try { thrower(1); thrower(5); } catch (const std::exception &e) { caught = std::string(e.what()) == "boom"; }
    CHECK(caught);
    int nested = 0;
    try {
        try { throw 7; } catch (int i) { nested = i; throw; }
    } catch (int j) { nested += j; }
    CHECK(nested == 14);
    printf("%s\n", fails ? "SOME TESTS FAILED" : "ALL PASSED");
    return fails;
}
