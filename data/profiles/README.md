# Temperature profiles

Save each temperature profile in this folder. A profile is an ordered set of points:

```csv
minutes,target_c
0,25
30,50
75,65
```

- `minutes` is elapsed time from the moment the profile starts.
- `target_c` is the desired target temperature in degrees Celsius.
- Times must be non-negative and strictly increasing.

The temperature controller holds the first target before its first timestamp, linearly interpolates targets between adjacent points, and holds the final target after the final timestamp.

Upload the filesystem image after adding a CSV, then choose it from the dashboard's **Temperature Profile** control. The controller supports up to 64 points per active profile.
