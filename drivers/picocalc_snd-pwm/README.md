Require hardware modification

    1.cut the metal pin of GPIO4_B3 on luckfox lyra
    2.solder a wire from GP2 to GP27(PWM_R) on PicoCalc

![IMG_20250406_111909](https://github.com/user-attachments/assets/13e0292c-dabb-4df7-848b-00bd97503ad1)
![IMG_20250406_111938](https://github.com/user-attachments/assets/0f97c4cb-1fb3-41f6-89ad-25703fafab0d)

Now RMIO_12 can be used to output audio via hardware pwm.

[https://www.youtube.com/shorts/p9XIFrtVFWE](https://www.youtube.com/shorts/p9XIFrtVFWE)

Device Tree

    / {
            compatible = "rockchip,rk3506";

            picocalc_snd {
                    status = "okay";
                    compatible = "fsl,picocalc-snd-pwm";
                    pinctrl-names = "default";
                    pwm-names = "pwm-snd-left";
                    pwms = <&pwm0_4ch_0 0 25000 0>;
            };
    };

    &pwm0_4ch_0 {
            pinctrl-names = "active";
            pinctrl-0 = <&rm_io12_pwm0_ch0>;
            status = "okay";
            assigned-clocks = <&cru CLK_PWM0>;
            assigned-clock-rates = <100000000>;
    };

/etc/asound.conf

    defaults.pcm.card 0
    defaults.ctl.card 0
