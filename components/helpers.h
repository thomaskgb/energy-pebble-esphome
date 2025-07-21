#include "esphome.h"
#include <NeoPixelBus.h>

// Helper function to convert color code to RGB value
RgbColor getColorValue(const std::string& color) {
  if (color == "G") {
    return RgbColor(0, 100%, 0);  // Green
  } else if (color == "Y") {
    return RgbColor(100%, 100%, 0);  // Yellow
  } else if (color == "R") {
    return RgbColor(100%, 0, 0);  // Red
  } else {
    return RgbColor(0, 0, 0);  // Off/Black
  }
}

// Helper function to get human-readable color name
std::string getColorName(const std::string& color) {
  if (color == "G") {
    return "Green (Cheapest)";
  } else if (color == "Y") {
    return "Yellow (Medium)";
  } else if (color == "R") {
    return "Red (Expensive)";
  } else {
    return "Unknown";
  }
}