#pragma once

namespace dxup {

  template <typename T, size_t Size>
  struct Vector {
    T data[Size];

    T& operator[](size_t index) {
      return data[index];
    }

    const T& operator[](size_t index) const {
      return data[index];
    }
  };

}