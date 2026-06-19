# Notes Zephyr H747: Devicetree, Settings et Memoire

Ce document complete le README principal avec quelques points importants rencontres pendant la mise au point de la phase 1.

## Devicetree: difference entre `.dts`, `.dtsi` et `.overlay`

### `.dts`

- Fichier principal d'une board ou d'une cible.
- Decrit une configuration concrete complete.
- Contient typiquement les `chosen`, les aliases, les peripheriques actives et des proprietes de board.

Exemple dans Zephyr:

- `zephyr/boards/st/stm32h747i_disco/stm32h747i_disco_stm32h747xx_m7.dts`

### `.dtsi`

- Fragment reutilisable inclus par un ou plusieurs `.dts`.
- Sert a factoriser la description d'un SoC, d'une famille de MCU, d'un pinctrl ou d'une base commune de carte.
- Un `.dts` inclut souvent plusieurs `.dtsi`.

Exemples dans Zephyr:

- `zephyr/dts/arm/st/h7/stm32h747Xi_m7.dtsi`
- `zephyr/boards/st/stm32h747i_disco/stm32h747i_disco.dtsi`

### `.overlay`

- Patch local applique par le projet sur le devicetree de la board.
- Permet d'adapter la configuration sans modifier directement Zephyr upstream.
- C'est la bonne couche pour changer un `chosen`, un `status`, un pinctrl, ou une partition flash specifique au projet.

Exemple dans ce projet:

- `app/boards/stm32h747i_disco_stm32h747xx_m7.overlay`

### Ordre logique

```text
.dtsi  -> briques reutilisables SoC / famille / base board
.dts   -> description principale de la board cible
.overlay -> surcharge locale du projet
```

## Vue concrete dans ce projet

- `stm32h747Xi_m7.dtsi` decrit la vue Cortex-M7 du MCU.
- `stm32h747i_disco_stm32h747xx_m7.dts` decrit la board STM32H747I-DISCO cote M7.
- `stm32h747i_disco_stm32h747xx_m7.overlay` applique les ajustements locaux du projet.

Point important:

- `stm32h747Xi_m7.dtsi` contient `/delete-node/ &flash1;`
- Donc pour la target `stm32h747i_disco/stm32h747xx/m7`, Zephyr n'expose que `flash0` de 1 MiB a l'image M7.

## Pourquoi le build affiche 1 MiB de flash et 512 KiB de RAM

La puce STM32H747 dispose physiquement de:

- 2 MiB de flash
- environ 1 MiB de RAM totale

Mais l'image Zephyr M7 ne voit pas toute cette memoire comme un seul bloc.

### Flash

- Le DTS M7 supprime `flash1`.
- La region de flash visible par le build M7 est donc `flash0` de 1 MiB.
- La ligne du build `FLASH: 1 MB` correspond a la region flash disponible pour cette image, pas a toute la flash physique de la puce.

### RAM

La RAM physique est repartie en plusieurs blocs distincts:

- AXI SRAM: 512 KiB
- DTCM: 128 KiB
- ITCM: 64 KiB
- SRAM1: 128 KiB
- SRAM2: 128 KiB
- SRAM3: 32 KiB
- SRAM4: 64 KiB

La ligne `RAM: 512 KB` du build correspond a la region RAM principale utilisee par defaut, et non a toute la RAM cumulee du MCU.

## Settings: difference entre NVS et FCB

Zephyr `settings` est une couche de persistance. Le backend peut changer selon la geometrie flash et l'usage.

### NVS

- Type cle/valeur.
- Faible overhead.
- Bien adapte a quelques parametres persistants.
- Compact et simple pour des petites configurations.

Limite rencontree ici:

- Sur STM32H747, les secteurs flash internes font 128 KiB.
- Le backend `settings_nvs` de Zephyr rejette cette geometrie dans notre configuration, donc `settings_subsys_init()` echouait.

### FCB

- `Flash Circular Buffer`.
- Fonctionne comme un journal append-only en flash.
- Plus tolerant a certaines geometries de flash et tailles de secteurs.
- Utile pour des logs ou des donnees append-only, mais moins naturel que NVS
  pour quelques parametres cle/valeur.

Conclusion pratique pour ce projet:

- NVS est le backend retenu pour la persistance des settings de la phase 1.
- Le stockage settings est place sur la QSPI externe, pas dans la flash interne.

## Partition settings actuelle: QSPI externe

La board Zephyr fournit par defaut une petite partition de stockage dans la flash interne:

- `storage_partition: partition@ff800`
- taille: 2 KiB

Cette partition interne n'est pas adaptee a notre usage:

- 2 KiB est trop petit pour le backend NVS tel qu'utilise ici.
- La flash interne STM32H747 s'efface par secteurs de 128 KiB.
- Garder les settings en flash interne consommerait un gros secteur pour quelques octets de configuration.

Le projet deplace donc explicitement `storage_partition` dans la QSPI externe via:

- `app/boards/stm32h747i_disco_stm32h747xx_m7.overlay`
- `zephyr,settings-partition = &storage_partition` dans le noeud `/chosen`
- `CONFIG_FLASH_STM32_QSPI=y` dans `app/prj.conf`

Layout actuel:

```dts
/ {
	chosen {
		zephyr,settings-partition = &storage_partition;
	};
};

&flash0 {
	partitions {
		/delete-node/ partition@ff800;
	};
};

&mt25ql512ab1 {
	partitions {
		/delete-node/ partition@0;

		storage_partition: partition@0 {
			label = "storage";
			reg = <0x00000000 DT_SIZE_K(16)>;
		};

		external_flash_partition: partition@4000 {
			label = "external-flash";
			reg = <0x00004000 (DT_SIZE_M(64) - DT_SIZE_K(16))>;
		};
	};
};
```

Details importants:

- la partition interne `partition@ff800` est supprimee de `flash0`
- la partition exemple `partition@0` fournie dans la QSPI est supprimee
- `storage_partition` est recreree au debut de `&mt25ql512ab1`
- `storage_partition` fait 16 KiB, soit deux secteurs NVS de 8 KiB avec la geometrie exposee par le driver QSPI STM32
- `external_flash_partition` reserve le reste de la QSPI pour les phases suivantes

Effet pratique:

- la flash interne `flash0` n'est plus amputee pour quelques octets de configuration IP
- l'application M7 conserve le maximum de place en flash interne
- le stockage persistant utilise une petite tranche de la QSPI externe

## Attention pour les evolutions futures

La QSPI externe sera aussi utile pour les prochaines phases:

- LittleFS pour les pages web et certificats
- slot secondaire MCUboot
- stockage de diagnostics ou assets

Quand ces phases arriveront, il faudra remplacer le layout temporaire `external_flash_partition` par un layout explicite, par exemple `settings`, `littlefs`, `slot1`, et eventuellement `scratch`.
