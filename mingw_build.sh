

g++ ./src/main.cpp -o ./build/final/get_book.exe -O3 --std=c++23 $(pkg-config --define-prefix --static --libs libcurl)
