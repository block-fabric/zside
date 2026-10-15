# The headers of Microsoft Edge WebView2 for chains-app on Windows (the Microsoft.Web.WebView2
# NuGet package, BSD-3-Clause, see LICENSE.txt in it). Only WebView2.h is used: the loader of
# webview/webview finds the WebView2 runtime itself (WEBVIEW_MSWEBVIEW2_BUILTIN_IMPL), so neither
# WebView2Loader.dll nor WebView2LoaderStatic.lib (an MSVC library) is shipped.
package=webview2
$(package)_version=1.0.4191.47
$(package)_download_path=https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$($(package)_version)/
$(package)_file_name=microsoft.web.webview2.$($(package)_version).nupkg
$(package)_sha256_hash=f492bbf547d0da329553b6727435b677579b1e9f91cc9e4a1ad029366d5f23d0

# A NuGet package is a zip file.
define $(package)_extract_cmds
  mkdir -p $($(package)_extract_dir) && \
  echo "$($(package)_sha256_hash)  $($(package)_source)" > $($(package)_extract_dir)/.$($(package)_file_name).hash && \
  $(build_SHA256SUM) -c $($(package)_extract_dir)/.$($(package)_file_name).hash && \
  unzip -q -o $($(package)_source) build/native/include/WebView2.h build/native/include/WebView2EnvironmentOptions.h LICENSE.txt NOTICE.txt
endef

# In a folder of its own: nothing else picks these headers up by accident.
define $(package)_stage_cmds
  mkdir -p $($(package)_staging_prefix_dir)/include/webview2 $($(package)_staging_prefix_dir)/share/webview2 && \
  cp build/native/include/WebView2.h build/native/include/WebView2EnvironmentOptions.h $($(package)_staging_prefix_dir)/include/webview2/ && \
  cp LICENSE.txt NOTICE.txt $($(package)_staging_prefix_dir)/share/webview2/ && \
  printf '%s\n' '/* WebView2.h includes EventToken.h: mingw-w64 names it eventtoken.h. */' '#include <eventtoken.h>' > $($(package)_staging_prefix_dir)/include/webview2/EventToken.h
endef
