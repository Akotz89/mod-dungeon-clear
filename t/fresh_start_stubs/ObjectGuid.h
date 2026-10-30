
#pragma once
#include <cstdint>
struct ObjectGuid {
 uint32_t id=0; ObjectGuid(uint32_t value=0):id(value){}
 uint32_t GetCounter()const{return id;}uint64_t GetRawValue()const{return id;} void Clear(){id=0;} bool IsEmpty()const{return id==0;}
 bool operator==(ObjectGuid const&)const=default; bool operator<(ObjectGuid const& other)const{return id<other.id;}
 static const ObjectGuid Empty;
};
inline const ObjectGuid ObjectGuid::Empty{};
