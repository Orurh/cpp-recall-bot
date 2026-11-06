#pragma once

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>

#include <cstdint>

// Application navigation, not a common educational/progress engine.
[[nodiscard]] inline boost::json::array AvailableEducationDirections(std::int64_t user_id, std::int64_t owner_id) {
  boost::json::array directions{boost::json::object{{"id", "cpp"}, {"title", "C++ Engineering"}}};
  if (owner_id > 0 && user_id == owner_id) {
    directions.emplace_back(boost::json::object{{"id", "system_design"}, {"title", "System Design"}});
  }
  return directions;
}
