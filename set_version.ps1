$version = (cat version);
$date = get-date -format MM\/dd\/yyyy;
# TODO: perhaps use stampinf?
cat qvideo\inf\qvideo.inf | %{$_ -replace "^DriverVer = .*","DriverVer = $date,$version" } > qvideo\inf\qvideo.inf.version
$version2 = %{$version -replace ".",","}
cat qvideo\inc\qubes_common.rc.template | %{$_ -replace "VER_PRODUCTVERSION	.*","VER_PRODUCTVERSION $version"} > qvideo\inc\qubes_common.rc
