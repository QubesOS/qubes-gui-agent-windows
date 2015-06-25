$version = Get-Content version
# qubes version has 3 parts, windows needs 4
$version += ".0"
$date = (get-date).ToUniversalTime() | get-date -format MM\/dd\/yyyy;
# TODO: perhaps use stampinf?
cat qvideo\inf\qvideo.inf | %{$_ -replace "^DriverVer = .*","DriverVer = $date,$version" } > qvideo\inf\qvideo.inf.version
$version2 = %{$version -replace "\.",","}
cat qvideo\inc\qubes_common.rc.template | %{$_ -replace "VER_PRODUCTVERSION	.*","VER_PRODUCTVERSION $version2"} > qvideo\inc\qubes_common.rc

$version = Get-Content version
$version_str = "`"" + $version + "`""
$version = %{$version -replace "\.", ","}
$hdr = "#define QTW_FILEVERSION " + $version + "`n"
$hdr += "#define QTW_FILEVERSION_STR " + $version_str + "`n"
$hdr += "#define QTW_PRODUCTVERSION 3.0.0.0`n"
$hdr += "#define QTW_PRODUCTVERSION_STR `"3.0.0.0`"`n"
Set-Content -Path "version.h" $hdr
