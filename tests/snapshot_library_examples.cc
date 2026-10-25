#include <algorithm>
#include <cassert>
#include <functional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

// Executable checks complement, but do not replace, the N4950 content review.
int main() {
  struct Item {
    int id;
  };
  Item item{3};
  std::invoke(&Item::id, item) = 9;
  assert(item.id == 9);
  struct Counter {
    int base;
    int Add(int n) const { return base + n; }
  };
  const Counter counter{6};
  assert(std::invoke(&Counter::Add, counter, 4) == 10);
  struct Record {
    std::string name;
  };
  static_assert(std::is_same_v<std::invoke_result_t<decltype(&Record::name), const Record&>, const std::string&>);
  static_assert(std::is_same_v<std::remove_cvref_t<const std::string&>, std::string>);
  static_assert(std::is_same_v<std::remove_cvref_t<const int* const&>, const int*>);
  std::vector<int> values{4, 9, 9, 15};
  assert(std::lower_bound(values.begin(), values.end(), 9) - values.begin() == 1);
  std::vector<int> descending{90, 60, 30};
  assert(*std::lower_bound(descending.begin(), descending.end(), 75, std::greater<>{}) == 60);
  std::vector<int> absent{4, 8, 12};
  assert(*std::lower_bound(absent.begin(), absent.end(), 6) == 8);
  int array[]{5, 11};
  std::span<const int> view = array;
  array[1] = 18;
  assert(view[1] == 18);
  const std::span<int> mutable_view{array};
  mutable_view[0] = 17;
  assert(array[0] == 17);
  struct Key {
    int group;
    int id;
  };
  const auto comp = [](const Key& a, const Key& b) {
    return a.group < b.group;
  };
  const Key a{2, 10}, b{2, 99};
  assert(a.id != b.id);
  assert(!comp(a, b) && !comp(b, a));
  struct Row {
    int score;
    char name;
  };
  std::vector<Row> rows{{3, 'c'}, {1, 'a'}, {2, 'b'}};
  std::ranges::sort(rows, {}, &Row::score);
  assert(rows[0].name == 'a' && rows[1].name == 'b' && rows[2].name == 'c');
}
