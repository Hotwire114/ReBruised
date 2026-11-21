# Scripted pad input that goes from boot into a fight (used for run ar4 and the speed runs): returns the
# BNB_PAD_SCRIPT string for tools\shots.ps1 -Pad. Needs BNB_MOVIE_TIMEOUT=40. The fight starts around 83 s.
#   $pad = & tools\fight_pad.ps1
((0..10 | ForEach-Object { "{0}:1000:0.25" -f (5 + $_) }) -join ';') + ';' +
((0..35 | ForEach-Object { $t = 20 + $_ * 4; "{0}:10:0.2;{1}:1000:0.2" -f $t, ($t + 2) }) -join ';') + ';' +
((0..44 | ForEach-Object { "{0}:1000:0.2" -f (162 + $_ * 2) }) -join ';')
