# rpi3-gpio-driver

Un petit driver Linux pour piloter une LED et un bouton poussoir sur un
Raspberry Pi 3, avec un overlay Device Tree pour le lier au hardware et
un programme de démonstration en userspace. Écrit comme pièce
de portfolio pour démontrer le modèle de driver Linux moderne : platform
driver + Device Tree + API descripteurs `gpiod` + interruption threadée
+ character device pour le userspace.

## Arborescence du repo

```
rpi3-gpio-driver/
├── driver/
│   ├── rpi3_gpio_driver.c   # platform driver (kernel module)
│   └── Makefile             # Makefile kbuild hors-arbre
├── dts/
│   └── rpi3-gpio-led-button-overlay.dts   # overlay Device Tree
└── userspace/
    └── gpio_demo.c          # exemple de client (poll/read/write)
```

## Câblage hardware

GPIO17 (broche physique 11) pilote la LED, GPIO27 (broche physique 13)
lit le bouton. Le bouton s'appuie sur le pull-up interne du SoC
(configuré dans l'overlay Device Tree, voir plus bas) : au repos la
ligne est haute, un appui la tire à la masse.

```mermaid
flowchart LR
    subgraph RPi3["Raspberry Pi 3 - connecteur GPIO"]
        PIN11["Broche 11 / GPIO17"]
        PIN13["Broche 13 / GPIO27"]
        GND1["Broche 9 / GND"]
        GND2["Broche 14 / GND"]
    end

    PIN11 -->|"résistance 330 ohms"| LED((Anode LED))
    LED -->|cathode| GND1

    BTN((Bouton poussoir)) --- PIN13
    BTN --- GND2

    style LED fill:#ffdd57,stroke:#333,color:#000
    style BTN fill:#8ecae6,stroke:#333,color:#000
```

**Pourquoi GPIO17 et GPIO27 précisément** : ce sont des broches
génériques, sans fonction alternative câblée en dur ailleurs sur le
connecteur (contrairement à GPIO0/1 réservées à l'EEPROM d'identification
des HAT, GPIO2/3 à l'I2C1, GPIO7-11 au SPI0, GPIO14/15 à l'UART0,
GPIO18-21 au PCM/I2S). N'importe quelle autre paire de GPIO libres aurait
fonctionné de façon électriquement identique. Ce choix est une pure
convention, reprise de la quasi-totalité des tutoriels RPi LED+bouton,
pas une contrainte hardware. L'intérêt concret est surtout d'éviter
tout conflit si on active un jour I2C/SPI/UART sur la même carte.

## Architecture logicielle

L'overlay ajoute un nœud (`compatible = "pompote,rpi3-led-button"`) que
le platform driver reconnaît. Une fois lié, `gpiod` fournit au driver
deux descripteurs GPIO ; celui du bouton est en plus mappé sur une IRQ.
Le userspace ne dialogue avec le driver qu'au travers de
`/dev/rpi3gpio0`.

Le contrôleur GPIO n'est pas une puce séparée (pas de type expandeur
I2C externe) : il fait partie intégrante du SoC BCM2837 lui-même, au
même titre que les cœurs ARM et le GPU VideoCore. Chaque broche
physique est multiplexable via pinmux entre plusieurs fonctions (GPIO
générique, I2C, SPI, UART, PWM...). C'est un peu comme un south-bridge
PC qui centralise les entrées/sorties bas débit, mais c'est intégré sur
la même puce, pas en silicium à part.

```mermaid
flowchart TB
    subgraph HW["Hardware"]
        LEDHW[LED sur GPIO17]
        BTNHW[Bouton sur GPIO27]
        SOC[Contrôleur GPIO du BCM2837]
    end

    subgraph KERNEL["Linux kernel"]
        PINCTRL[pinctrl-bcm2835]
        GPIOLIB[gpiolib / API gpiod]
        IRQCHIP[irq-bcm2835]
        OF["Nœud de l'overlay Device Tree<br/>compatible = pompote,rpi3-led-button"]
        PDRV[rpi3_gpio_driver.ko<br/>platform_driver]
        MISC["misc device /dev/rpi3gpio0"]
        VFS[VFS - couche syscalls]
    end

    subgraph USER["Userspace"]
        SHELL["shell : echo / cat"]
        DEMO["gpio_demo : open/read/write/poll"]
    end

    SOC --> PINCTRL --> GPIOLIB
    SOC --> IRQCHIP
    OF -->|"matché via of_match_table"| PDRV
    GPIOLIB <--> PDRV
    IRQCHIP <--> PDRV
    PDRV --> MISC --> VFS
    VFS <--> SHELL
    VFS <--> DEMO
    PDRV -. pilote .-> LEDHW
    BTNHW -. interruption .-> PDRV
```

### Chargement du module / probe

```mermaid
sequenceDiagram
    participant U as root (insmod)
    participant K as Chargeur de modules
    participant PB as Bus platform
    participant DRV as rpi3_gpio_driver
    participant GPIOLIB as gpiolib
    participant IRQ as Sous-système IRQ
    participant MISC as Framework misc

    U->>K: insmod rpi3_gpio_driver.ko
    K->>DRV: module_init -> platform_driver_register
    Note over PB: overlay déjà chargé,<br/>nœud "pompote,rpi3-led-button" présent
    PB->>DRV: correspondance of_match_table -> probe()
    DRV->>GPIOLIB: devm_gpiod_get("led", OUT_LOW)
    GPIOLIB-->>DRV: gpio_desc (GPIO17)
    DRV->>GPIOLIB: devm_gpiod_get("button", IN)
    GPIOLIB-->>DRV: gpio_desc (GPIO27)
    DRV->>GPIOLIB: gpiod_to_irq(button)
    GPIOLIB-->>DRV: numéro d'IRQ
    DRV->>IRQ: devm_request_threaded_irq(isr, IRQF_ONESHOT)
    IRQ-->>DRV: ok
    DRV->>MISC: misc_register(rpi3gpio0)
    MISC-->>DRV: /dev/rpi3gpio0 créé
    DRV-->>U: dev_info "loaded, control via /dev/rpi3gpio0"
```

### Piloter la LED depuis le userspace (chemin d'écriture)

```mermaid
sequenceDiagram
    participant App as Programme utilisateur
    participant VFS as VFS
    participant DRV as rpi3_gpio_driver (.write)
    participant GPIOLIB as gpiolib
    participant HW as GPIO17 (LED)

    App->>VFS: write(fd, "1", 1)
    VFS->>DRV: callback .write()
    DRV->>DRV: copy_from_user
    DRV->>GPIOLIB: gpiod_set_value_cansleep(led, 1)
    GPIOLIB->>HW: bascule le registre GPSET/GPCLR
    HW-->>App: la LED s'allume (effet physique)
    DRV-->>VFS: retourne 1 (octet consommé)
    VFS-->>App: write() retourne 1
```

### Interruption bouton / chemin de lecture

```mermaid
sequenceDiagram
    participant HW as Bouton (GPIO27)
    participant IRQC as irq-bcm2835
    participant ISR as rpi3_gpio_button_isr (threadée)
    participant WQ as wait_queue
    participant App as Userspace (read/poll bloquant)

    App->>WQ: read(fd, buf, N) -> wait_event_interruptible
    Note over App,WQ: le process dort, pas d'attente active

    HW->>IRQC: front descendant (appui)
    IRQC->>ISR: réveille le thread d'IRQ
    ISR->>ISR: anti-rebond (jiffies), relit la valeur gpiod
    ISR->>ISR: event = "PRESSED\n"
    ISR->>WQ: wake_up_interruptible()
    WQ-->>App: read() se débloque, copy_to_user("PRESSED\n")
    App->>App: traite l'évènement (ex. pilote la LED)
```

## Compilation

Deux artefacts indépendants à compiler : le kernel module et l'overlay
Device Tree.

### Kernel module

```sh
cd driver
# Build natif, exécuté directement sur le Pi (nécessite raspberrypi-kernel-headers) :
make ARCH= CROSS_COMPILE=

# Cross-compilation sous WSL2 contre une arborescence raspberrypi/linux
# configurée et compilée, correspondant au kernel réellement lancé sur la cible :
make KDIR=/chemin/vers/raspberrypi/linux
```

Ceci produit `rpi3_gpio_driver.ko`.

### Overlay Device Tree

```sh
cd dts
dtc -@ -I dts -O dtb -o rpi3-gpio-led-button.dtbo rpi3-gpio-led-button-overlay.dts
```

`-@` conserve les informations de phandle/symbole de l'overlay,
nécessaires pour qu'un overlay `/plugin/` soit résolvable au chargement.

## Déploiement sur le Pi

1. Copier `rpi3_gpio_driver.ko` et `rpi3-gpio-led-button.dtbo` sur le Pi.
2. Charger l'overlay, de façon persistante ou pour un test ponctuel :

   ```sh
   # Persistant (survit au reboot) : copier le .dtbo, puis le référencer
   sudo cp rpi3-gpio-led-button.dtbo /boot/overlays/
   echo "dtoverlay=rpi3-gpio-led-button" | sudo tee -a /boot/config.txt
   sudo reboot

   # OU dynamique, sans reboot (nécessite que le .dtbo soit déjà dans /boot/overlays/) :
   sudo dtoverlay rpi3-gpio-led-button
   ```

3. Charger le module :

   ```sh
   sudo insmod rpi3_gpio_driver.ko
   dmesg | tail        # confirme que le probe a réussi, /dev/rpi3gpio0 créé
   ```

## Piloter depuis le userspace

```mermaid
flowchart TD
    A["Compiler le module : make dans driver/"] --> B["Compiler l'overlay : dtc -@ ..."]
    B --> C[Copier .ko + .dtbo sur le Pi3]
    C --> D["Charger l'overlay : dtoverlay, ou config.txt + reboot"]
    D --> E[sudo insmod rpi3_gpio_driver.ko]
    E --> F{/dev/rpi3gpio0 présent ?}
    F -- non --> G[Vérifier dmesg : probe échoué ?]
    F -- oui --> H["echo 1 > /dev/rpi3gpio0 -> LED allumée"]
    H --> I["cat /dev/rpi3gpio0 -> bloque jusqu'au prochain appui"]
    I --> J["./gpio_demo -> boucle poll : la LED suit l'état du bouton"]
```

Une fois `/dev/rpi3gpio0` présent :

```sh
# Allumer / éteindre la LED
echo 1 | sudo tee /dev/rpi3gpio0
echo 0 | sudo tee /dev/rpi3gpio0

# Bloque jusqu'au prochain appui/relâchement du bouton, l'affiche, puis quitte
sudo cat /dev/rpi3gpio0
```

Ou lancer le programme de démo, qui utilise `poll()` pour réagir aux
évènements du bouton sans attente active, et fait suivre l'état du
bouton sur la LED :

```sh
gcc -o gpio_demo userspace/gpio_demo.c
sudo ./gpio_demo
```

## Dépannage

- **`/dev/rpi3gpio0` absent** : `dmesg | grep rpi3-gpio-driver`. Le plus
  probable est que l'overlay n'est pas chargé (aucun nœud DT
  correspondant, donc `probe()` ne s'exécute jamais) ou que
  `devm_gpiod_get()` a échoué (vérifier que les noms de propriétés
  `led-gpios` / `button-gpios` de l'overlay correspondent à ce que le
  driver demande).
- **Permission refusée sur `/dev/rpi3gpio0`** : les misc devices sont
  root-only par défaut sauf règle udev spécifique ; utiliser `sudo` ou
  ajouter une règle.
- **Le bouton ne déclenche jamais rien** : vérifier le câblage par
  rapport au [schéma de câblage](#câblage-hardware), et que
  `button_pins` dans l'overlay active bien le pull-up
  (`brcm,pull = <2>`). Sans ça, l'entrée flotte et les interruptions se
  déclenchent aléatoirement.

## Licence

GPL-2.0-only (kernel module et overlay Device Tree) ; la démo en
userspace est fournie sous la même licence par cohérence.

