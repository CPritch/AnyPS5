#ifndef CORE_SHADER_RECOMPILER_COWVECTOR_HPP
#define CORE_SHADER_RECOMPILER_COWVECTOR_HPP

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

namespace ShaderRecompiler {

template<typename T>
class CowVector {
public:
    using value_type = T;
    using size_type = std::size_t;
    using const_reference = typename std::vector<T>::const_reference;
    using const_iterator = typename std::vector<T>::const_iterator;

    CowVector() = default;
    CowVector(std::initializer_list<T> values) : CowVector(std::vector<T>(values)) {}
    CowVector(std::vector<T> values) : values(values.empty() ? nullptr : std::make_shared<std::vector<T>>(std::move(values))) {}
    CowVector(size_type count, const T& value) : CowVector(std::vector<T>(count, value)) {}

    CowVector& operator=(std::initializer_list<T> replacement) { return *this = std::vector<T>(replacement); }

    CowVector& operator=(std::vector<T> replacement) {
        values = replacement.empty() ? nullptr : std::make_shared<std::vector<T>>(std::move(replacement));
        return *this;
    }

    const std::vector<T>& Get() const { return values != nullptr ? *values : None(); }
    operator const std::vector<T>&() const { return Get(); }

    std::vector<T>& Mutable() {
        if (values == nullptr) values = std::make_shared<std::vector<T>>();
        else if (values.use_count() > 1) values = std::make_shared<std::vector<T>>(*values);
        return *values;
    }

    size_type size() const { return values != nullptr ? values->size() : 0; }
    bool empty() const { return size() == 0; }
    const T* data() const { return Get().data(); }
    const_reference operator[](size_type index) const { return (*values)[index]; }
    const_reference at(size_type index) const { return Get().at(index); }
    const_reference front() const { return values->front(); }
    const_reference back() const { return values->back(); }
    const_iterator begin() const { return Get().begin(); }
    const_iterator end() const { return Get().end(); }
    const_iterator cbegin() const { return Get().cbegin(); }
    const_iterator cend() const { return Get().cend(); }

    void push_back(const T& value) { Mutable().push_back(value); }
    template<typename... Arguments>
    decltype(auto) emplace_back(Arguments&&... arguments) { return Mutable().emplace_back(std::forward<Arguments>(arguments)...); }
    void resize(size_type count) { Mutable().resize(count); }
    void resize(size_type count, const T& value) { Mutable().resize(count, value); }
    void assign(size_type count, const T& value) { Mutable().assign(count, value); }
    template<typename Iterator>
    void assign(Iterator first, Iterator last) { Mutable().assign(first, last); }
    void reserve(size_type count) { Mutable().reserve(count); }
    void clear() { values.reset(); }

    friend bool operator==(const CowVector& left, const CowVector& right) { return left.values == right.values || left.Get() == right.Get(); }
    friend bool operator==(const CowVector& left, const std::vector<T>& right) { return left.Get() == right; }

private:
    static const std::vector<T>& None() {
        static const std::vector<T> none;
        return none;
    }

    std::shared_ptr<std::vector<T>> values;
};

}

#endif
