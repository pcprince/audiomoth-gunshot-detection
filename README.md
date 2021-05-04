# audiomoth-gunshot-detection

AudioMoth firmware based around a hidden Markov model designed to detect the presence of gunshots in a protected reserve in Belize. The model was trained on gunshot and background noise recordings collected between 2017 and 2019.

When detected, the AudioMoth will make a single 4-second recording, containing the audio which triggered it. Designed for longterm deployment, this firmware was deployed on devices which were deployed throughout the reserve.

The model was deployed on older AudioMoth hardware with a model trained for the specific application location. As a result, it would require reworking to effectively use this repository with a modern AudioMoth in other applications. 

Described in detail in: [Deploying Acoustic Detection Algorithms on Low-Cost, Open-Source Acoustic Sensors for Environmental Monitoring](https://www.mdpi.com/1424-8220/19/3/553), by P. Prince, A. Hill, E. P. Covarrubias, P. Doncaster, J. L. Snaddon, and A. Rogers.
