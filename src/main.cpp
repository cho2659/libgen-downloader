#include <curl/curl.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <regex>
#include <cstring>

struct MemBuf {
    char* data;
    size_t size;
};


std::string trim(const std::string& str) {
    const std::string whitespace = " \t\n\r\f\v"; // Common whitespace characters
    size_t first = str.find_first_not_of(whitespace);
    if (std::string::npos == first) {
        return str; // No non-whitespace characters found
    }
    size_t last = str.find_last_not_of(whitespace);
    return str.substr(first, (last - first + 1));
}

size_t write_cb(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    MemBuf* mem = (MemBuf*)userp;
    char* ptr = (char*)realloc(mem->data, mem->size + total + 1);
    if (!ptr) return 0;
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, total);
    mem->size += total;
    mem->data[mem->size] = 0;
    return total;
}

size_t write_file(void* ptr, size_t size, size_t nmemb, void* userp) {
    FILE* stream = (FILE*)userp;
    return fwrite(ptr, size, nmemb, stream);
}

struct HeaderData {
    std::string filename;
};

size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t total = size * nitems;
    std::string header(buffer, total);
    HeaderData* hd = (HeaderData*)userdata;
    
    if (header.find("Content-Disposition:") != std::string::npos || 
        header.find("content-disposition:") != std::string::npos) {
        std::regex fname_pattern(R"(filename\*?=[\"']?([^\"'\r\n;]+)[\"']?)");
        std::smatch match;
        if (std::regex_search(header, match, fname_pattern)) {
            std::string raw = match[1].str();
            size_t utf_pos = raw.find("''");
            if (utf_pos != std::string::npos) {
                hd->filename = raw.substr(utf_pos + 2);
            } else {
                hd->filename = raw;
            }
        }
    }
    return total;
}

std::string fetch(CURL* curl, const std::string& url) {
    MemBuf chunk = {nullptr, 0};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&chunk);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    
    CURLcode res = curl_easy_perform(curl);
    std::string result;
    if (res == CURLE_OK && chunk.data) {
        result = std::string(chunk.data, chunk.size);
    }
    free(chunk.data);
    return result;
}

std::string extract_get_link(const std::string& html) {
    std::regex pattern(R"(<a[^>]+href=[\"']([^\"']+)[\"'][^>]*>\s*<h2>\s*GET\s*</h2>\s*</a>)", std::regex::icase);
    std::smatch match;
    if (std::regex_search(html, match, pattern)) {
        return match[1].str();
    }
    
    std::regex pattern2(R"(<a[^>]+href=[\"']([^\"']+)[\"'][^>]*>GET</a>)", std::regex::icase);
    if (std::regex_search(html, match, pattern2)) {
        return match[1].str();
    }
    return "";
}

std::string get_ext(const std::string& fname) {
    size_t dot = fname.find_last_of('.');
    if (dot != std::string::npos) {
        return fname.substr(dot);
    }
    return "";
}

std::string clean_filename(const std::string& fname) {
    std::string result = fname;
    
    size_t dash = result.find(" - libgen");
    if (dash != std::string::npos) {
        std::string ext = get_ext(result);
        result = result.substr(0, dash) + ext;
    }
    
    return result;
}


void download(CURL* curl, const std::string& url) {
    HeaderData hdata;
    FILE* fp = fopen("temp_download", "wb");
    if (!fp) {
        std::cerr << "Failed to open temp file\n";
        return;
    }
    
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hdata);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    
    CURLcode res = curl_easy_perform(curl);
    fclose(fp);
    
    if (res != CURLE_OK) {
        std::cerr << "Download failed\n";
        unlink("temp_download");
        return;
    }
    
    if (hdata.filename.empty()) {
        std::cerr << "Filename not found in headers\n";
        unlink("temp_download");
        return;
    }
    
    std::string final_name = clean_filename(hdata.filename);
    final_name = trim(final_name);
    
    if (rename("temp_download", final_name.c_str()) != 0) {
        std::cerr << "Failed to rename file\n";
        unlink("temp_download");
        return;
    }
    
    std::cout << "Downloaded: " << final_name << "\n";
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <link> [--socks5-proxy|--http-proxy <proxy>]\n";
        return 1;
    }
    
    std::string url = argv[1];
    std::string proxy, proxy_type;
    
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--socks5-proxy" || arg == "--http-proxy") {
            if (i + 1 < argc) {
                proxy_type = arg;
                proxy = argv[++i];
            }
        }
    }
    
    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL* curl = curl_easy_init();
    
    if (!curl) {
        std::cerr << "CURL init failed\n";
        return 1;
    }
    
    if (!proxy.empty()) {
        curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
        if (proxy_type == "--socks5-proxy") {
            curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5);
        } else {
            curl_easy_setopt(curl, CURLOPT_PROXYTYPE, CURLPROXY_HTTP);
        }
    }


    std::string from_replace = "ads";
    std::string to_replace = "get";

    size_t pos = url.find(from_replace);

    if (pos != std::string::npos) { // Check if the substring was found
        url.replace(pos, from_replace.length(), to_replace);
    } 
    std::cout << "Fetching: " << url << "\n";
    std::string html = fetch(curl, url);
    
    if (html.empty()) {
        std::cerr << "Failed to fetch page\n";
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        return 1;
    }
    
    std::string get_link = extract_get_link(html);
    if (get_link.empty()) {
        std::cerr << "GET link not found\n";
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        return 1;
    }
    
    if (get_link[0] == '/') {
        size_t proto_end = url.find("://");
        size_t domain_end = url.find('/', proto_end + 3);
        std::string base = (domain_end != std::string::npos) ? 
                          url.substr(0, domain_end) : url;
        get_link = base + get_link;
    } else if (get_link.find("://") == std::string::npos) {
        size_t proto_end = url.find("://");
        size_t domain_end = url.find('/', proto_end + 3);
        std::string base = (domain_end != std::string::npos) ? 
                          url.substr(0, domain_end) : url;
        get_link = base + "/" + get_link;
    }
    
    std::cout << "GET link: " << get_link << "\n";
    
    download(curl, get_link);
    
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return 0;
}


