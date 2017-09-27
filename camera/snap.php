<?php
//Grab circular queue of thumbnail snaps
	$queueLen = 4;
	$filename = "snap";
	if(file_exists($filename . $queueLen . ".jpg")) {
		unlink($filename . $queueLen . ".jpg");
	}
	for($i = $queueLen - 1; $i > 0; $i--) {
		if(file_exists($filename . $i . ".jpg")) {
			$j = $i + 1;
			rename($filename . $i . ".jpg",$filename . $j . ".jpg");
		}
	}
	copy("cam.jpg",$filename . "1.jpg");
	echo "done";
?>
