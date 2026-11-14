// café 🧭: offsets after this line are UTF-8 bytes.
namespace catalog {
template<class T> struct Record {
    T value;
    T shared() const { return value; }
};
using Count = Record<int>;
}
