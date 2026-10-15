#include "../src/gui_images.cpp"
#include <cassert>
#include <iostream>
int main(){using namespace lualoader::gui::images;
 // API handles are mocked; exercises production accounting and release logic.
 int freed=0;static int *counter=&freed;
 patchlib_type_get_type=[](const char*,const char*)->patch_handle_t{return nullptr;};
 patchlib_free=[](patch_handle_t){++*counter;};
 budget=32;frame=10;Entry old;old.path="old.png";old.last_use=1;old.resident=20;old.tex=reinterpret_cast<void*>(1);old.crops[1]=reinterpret_cast<void*>(2);cache.emplace("old",std::move(old));texture_bytes=20;
 Entry fresh;fresh.path="fresh.png";fresh.last_use=frame;cache.emplace("fresh",std::move(fresh));
 assert(make_room(24,&cache.at("fresh")));assert(texture_bytes==0&&evictions==1&&freed==2);assert(cache.count("old")&&!cache.at("old").attempted);
 cache.at("fresh").resident=20;texture_bytes=20;assert(!make_room(24,nullptr));assert(!make_room(33,nullptr));
 uploads=2;assert(!can_upload());begin_frame();assert(can_upload());uploaded(100);assert(can_upload());uploaded(100);assert(!can_upload());
 release("fresh");assert(texture_bytes==0);clear();assert(cache.empty());
 std::cout<<"PASS LRU, current-frame protection, stable descriptors, crop disposal, accounting, upload count budget\n";
}
